import Foundation

/// その人の書き方（文書から数えた読みごとの表記の回数、`NotationPreference`）を、モデルの採点に加える。
///
/// 候補 y の点数を「モデルの対数確率 log P(y | 読み, 左文脈) ＋ 重み × その人の書き方から見た不利さ」にする。
/// 不利さは y を分かち書きした語ごとの log((その表記の回数 + 1) / (その語の表記の違いで最も多い表記の回数 + 1)) の和
/// （`NotationPreference.penalty`。及び／および のような表記の違いだけに当て、精度／制度 のような別の語には当てない）。
/// 調整する値は重みだけで、0 なら何もしない（下の素通し）。
///
/// - 候補ウィンドウ（`score`）: 辞書ラティスの候補を並べる点数に不利さを足す（`LatticeRescoringEngine` の下に置く）
/// - 第一候補（`convert` で 1 候補）: モデルの答え y の語を 1 つずつ、その人が使った表記の違いに替えた文と、
///   全部を最も多い表記に替えた文を作り、y と合わせて上の点数で選び直す。替える語が無ければモデルを呼び直さない
///
/// 書き方の分かる語（読みが 2 拍以上で、その人が表記の違いを使い分けている語。決まった言い回しの位置は除く）
/// だけに効く。2026-11-07 の計測（論文 12 本、重み 2）: 論文の文 375 件で 225 → 231、文書と関係のない
/// iroha-bench-v1 で 2,248 → 2,244（壊した 15 件はほぼ著者の書き方への寄せ）、AJIMEE で 172 → 170
///
/// **話題で切り替える項**（`topicWeight`）: 同じ読みの別の語（記譜／寄付、符頭／不等）の選び分けは、
/// 入力の話題が文書に近いときだけ効かせる。左文脈と、候補のどれにも共通する語（＝選び方に関わらない語）から
/// 最も近い文書とその近さ s（入力の話題語のうちその文書に出る割合、0〜1、`DocumentTopicIndex`）を求め、その文書の語の使い方から見た不利さ
/// （`NotationPreference.wordPenalty`）に「話題の重み × s」をかけて足す。判定している語そのものを話題の根拠に
/// 使わないのは、使うと「符頭」を含む候補が自分で話題を作り、どの文でも文書の語に寄るため
public struct NotationPreferenceEngine<Base: ConversionEngine & CandidateScorer>: ConversionEngine, CandidateScorer {

    public let base: Base
    private let tokenizer: any ReadingTokenizer
    /// 変換のたびに読む（評価では文ごとに差し替える。IME では文書を読み直したら差し替える）
    private let configuration: @Sendable () -> NotationPreferenceConfiguration

    public init(base: Base, tokenizer: any ReadingTokenizer, configuration: @escaping @Sendable () -> NotationPreferenceConfiguration) {
        self.base = base
        self.tokenizer = tokenizer
        self.configuration = configuration
    }

    public func prewarm() async throws {
        try await base.prewarm()
    }

    public func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        let configuration = configuration()
        let results = try await base.convert(reading: reading, context: context, candidateCount: candidateCount)
        guard configuration.isActive, candidateCount <= 1, let generated = results.first, !generated.isEmpty else {
            return results
        }
        let candidates = rewriteCandidates(of: generated, context: context, configuration: configuration)
        guard candidates.count > 1 else { return results }
        try Task.checkCancellation()
        let scores = try await score(candidates: candidates, reading: reading, context: context, configuration: configuration)
        guard let best = zip(candidates, scores).max(by: { $0.1 < $1.1 }), best.1 > -.infinity else { return results }
        return [best.0]
    }

    public func score(candidates: [String], reading: String, context: String) async throws -> [Float] {
        let configuration = configuration()
        guard configuration.isActive else {
            return try await base.score(candidates: candidates, reading: reading, context: context)
        }
        return try await score(candidates: candidates, reading: reading, context: context, configuration: configuration)
    }

    private func score(
        candidates: [String], reading: String, context: String, configuration: NotationPreferenceConfiguration
    ) async throws -> [Float] {
        let modelScores = try await base.score(candidates: candidates, reading: reading, context: context)
        let tokenized = candidates.map { tokenizer.tokens(in: $0) }
        let topic = topicPreference(context: context, evidence: Self.sharedTokens(tokenized), configuration: configuration)
        return zip(tokenized, modelScores).map { tokens, modelScore in
            guard modelScore > -.infinity else { return modelScore }
            var bonus = 0.0
            if configuration.weight > 0 { bonus += configuration.weight * configuration.preference.penalty(tokens) }
            if let topic { bonus += configuration.topicWeight * topic.similarity * topic.preference.wordPenalty(tokens) }
            return modelScore + Float(bonus)
        }
    }

    /// 左文脈と `evidence` の語から、最も近い文書の語の使い方とその近さ。話題の項を使わない設定・どの文書とも
    /// 重ならないときは nil
    func topicPreference(
        context: String, evidence: [ReadingToken], configuration: NotationPreferenceConfiguration
    ) -> (preference: NotationPreference, similarity: Double)? {
        guard configuration.topicWeight > 0, let index = configuration.topicIndex,
              let nearest = index.nearest(tokenizer.tokens(in: context) + evidence),
              configuration.documentPreferences.indices.contains(nearest.index)
        else { return nil }
        return (configuration.documentPreferences[nearest.index], nearest.similarity)
    }

    /// 候補のどれにも同じ表記で出てくる語（1 つ目の候補の語の並びから選ぶ）。候補が 1 つならその語すべて
    static func sharedTokens(_ tokenized: [[ReadingToken]]) -> [ReadingToken] {
        guard let first = tokenized.first else { return [] }
        let others = tokenized.dropFirst().map { Set($0.map(\.surface)) }
        return first.filter { token in others.allSatisfy { $0.contains(token.surface) } }
    }

    /// モデルの答えと、その語を書き方の分かる表記に替えた文（重複なし、`maxRewriteCandidates` まで）
    func rewriteCandidates(
        of generated: String, context: String = "", configuration: NotationPreferenceConfiguration
    ) -> [String] {
        let tokens = tokenizer.tokens(in: generated)
        let preference = configuration.preference
        // 別の語を試すのは、話題が近い文書があるときだけ（どの語を試すかを決めるだけなので、答え全体を根拠にしてよい）
        let topic = topicPreference(context: context, evidence: tokens, configuration: configuration)
        var candidates = [generated]
        var seen: Set<String> = [generated]
        var favorites = tokens.map(\.surface)
        var hasFavorite = false
        var singles: [String] = []
        var wordSingles: [String] = []
        for (index, token) in tokens.enumerated() {
            guard let reading = token.reading, !NotationPreference.isFixedKanaPosition(tokens, index) else { continue }
            for alternative in topic?.preference.wordAlternatives(for: token.surface, reading: reading) ?? [] {
                var surfaces = tokens.map(\.surface)
                surfaces[index] = alternative
                wordSingles.append(surfaces.joined())
            }
            let alternatives = configuration.weight > 0 ? preference.alternatives(for: token.surface, reading: reading) : []
            guard !alternatives.isEmpty else { continue }
            if preference.penalty(surface: alternatives[0], reading: reading)
                > preference.penalty(surface: token.surface, reading: reading) {
                favorites[index] = alternatives[0]
                hasFavorite = true
            }
            for alternative in alternatives {
                var surfaces = tokens.map(\.surface)
                surfaces[index] = alternative
                singles.append(surfaces.joined())
            }
        }
        // 全部を最も多い表記にした文を先に、別の語を試す文をその次に入れる（上限で切れても残るように）
        singles.insert(contentsOf: wordSingles, at: 0)
        if hasFavorite { singles.insert(favorites.joined(), at: 0) }
        for candidate in singles where !seen.contains(candidate) {
            guard candidates.count < configuration.maxRewriteCandidates else { break }
            seen.insert(candidate)
            candidates.append(candidate)
        }
        return candidates
    }
}

/// `NotationPreferenceEngine` が変換のたびに読む設定
public struct NotationPreferenceConfiguration: Sendable {
    /// すべての文書を合わせて数えた表記の回数（表記の好みの項）
    public var preference: NotationPreference
    /// 表記の好みの不利さにかける重み。0 なら使わない
    public var weight: Double
    /// 文書ごとに数えた表記の回数（`topicIndex` と同じ並び。話題で切り替える項）
    public var documentPreferences: [NotationPreference]
    public var topicIndex: DocumentTopicIndex?
    /// 話題で切り替える項の重み（不利さ × 近さ にかける）。0 なら使わない
    public var topicWeight: Double
    /// 第一候補で試す文の上限（モデルの答えを含む）
    public var maxRewriteCandidates: Int

    public init(
        preference: NotationPreference, weight: Double,
        documentPreferences: [NotationPreference] = [], topicIndex: DocumentTopicIndex? = nil, topicWeight: Double = 0,
        maxRewriteCandidates: Int = 16
    ) {
        self.preference = preference
        self.weight = weight
        self.documentPreferences = documentPreferences
        self.topicIndex = topicIndex
        self.topicWeight = topicWeight
        self.maxRewriteCandidates = maxRewriteCandidates
    }

    public static let disabled = NotationPreferenceConfiguration(preference: .empty, weight: 0)

    var isActive: Bool {
        (weight > 0 && !preference.counts.isEmpty)
            || (topicWeight > 0 && topicIndex != nil && !documentPreferences.isEmpty)
    }
}
