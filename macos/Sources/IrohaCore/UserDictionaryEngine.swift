import Foundation

/// ユーザ辞書を反映させる変換エンジンのデコレータ。
///
/// LLMベースの変換エンジンには辞書を後から差し込む口がないため、読みの側で処理する:
/// - 読み全体がユーザ辞書に完全一致 → その単語を最優先の候補にする
/// - 読みの一部が一致 → 一致部分を単語で埋め、残りだけをエンジンに変換させて連結する
///   （例:「きららざかにいく」→「雲母坂」+ エンジンによる「にいく」→「に行く」）
///   一致を使うのは辞書ラティスで語の境目になる所だけ（`lattice` が無ければ読みが並べば使う）
///
/// ユーザ辞書が空のときは何もせず素通しする（既定のふるまいと完全に同じ）。
public final class UserDictionaryEngine: ConversionEngine {

    private let base: any ConversionEngine
    private let dictionaryProvider: @Sendable () -> UserDictionary
    /// 読みの途中の一致が語の境目に当たるかを決める辞書ラティス
    private let lattice: LatticeConverter?

    public init(
        base: any ConversionEngine,
        dictionary: @escaping @Sendable () -> UserDictionary = {
            UserDictionaryStore.shared.current
        },
        lattice: LatticeConverter? = nil
    ) {
        self.base = base
        self.dictionaryProvider = dictionary
        self.lattice = lattice
    }

    public func prewarm() async throws {
        try await base.prewarm()
    }

    public func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        let dictionary = dictionaryProvider()
        guard !dictionary.isEmpty else {
            return try await base.convert(
                reading: reading, context: context, candidateCount: candidateCount)
        }

        // ライブ変換・文節分割用（1候補）: 辞書を当てた結果をそのまま返す。
        // ただし出力が読みより大幅に長いエントリ（ハッシュタグ・定型文など）は
        // 文中に埋め込まれると邪魔なので使わない（候補ウィンドウにだけ出す）
        if candidateCount <= 1 {
            guard !dictionary.isEmptyForLiveConversion else {
                return try await base.convert(reading: reading, context: context, candidateCount: 1)
            }
            if let word = dictionary.liveWords(forReading: reading).first {
                InferenceTimer.current?.note(.userDictionary)
                return [word]
            }
            let chunks = dictionary.split(
                reading, forLiveConversion: true,
                wordBoundaries: await wordBoundaries(reading, dictionary: dictionary, forLiveConversion: true))
            guard Self.hasWordChunk(chunks) else {
                return try await base.convert(reading: reading, context: context, candidateCount: 1)
            }
            return [try await compose(chunks, context: context)]
        }

        // 候補ウィンドウ用: ユーザ辞書の単語を先頭に、続けてエンジンの候補を並べる
        let exactWords = dictionary.words(forReading: reading)
        let chunks = dictionary.split(
            reading, wordBoundaries: await wordBoundaries(reading, dictionary: dictionary, forLiveConversion: false))
        var results = exactWords
        if Self.hasWordChunk(chunks), exactWords.isEmpty {
            results.append(try await compose(chunks, context: context))
        }
        do {
            let baseCandidates = try await base.convert(
                reading: reading, context: context, candidateCount: candidateCount)
            for candidate in baseCandidates where !results.contains(candidate) {
                results.append(candidate)
            }
        } catch {
            // エンジンが失敗してもユーザ辞書の単語だけは出す
            if results.isEmpty { throw error }
        }
        return results
    }

    /// 読みの中に一致するエントリを辞書ラティスに入れて変換したときの語の境目。
    /// 一致するエントリが無い・ラティスが無いときは nil（ラティスを呼ばない）
    private func wordBoundaries(
        _ reading: String, dictionary: UserDictionary, forLiveConversion: Bool
    ) async -> Set<Int>? {
        guard let lattice else { return nil }
        let words = dictionary.entries(occurringIn: reading, forLiveConversion: forLiveConversion)
        guard !words.isEmpty else { return nil }
        return await lattice.wordBoundaries(reading: reading, userWords: words)
    }

    private static func hasWordChunk(_ chunks: [UserDictionary.Chunk]) -> Bool {
        chunks.contains { if case .word = $0 { return true } else { return false } }
    }

    /// 辞書一致部分はそのまま、それ以外はエンジンに変換させて連結する。
    /// 直前までの変換結果を次のチャンクの文脈として渡す
    private func compose(_ chunks: [UserDictionary.Chunk], context: String) async throws -> String {
        var result = ""
        var context = context
        for chunk in chunks {
            switch chunk {
            case .word(let word):
                result += word
                context += word
            case .reading(let reading):
                let converted = try await base.convert(
                    reading: reading, context: context, candidateCount: 1).first ?? reading
                result += converted
                context += converted
            }
        }
        return result
    }
}
