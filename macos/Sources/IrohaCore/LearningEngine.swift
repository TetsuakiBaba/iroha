import Foundation

/// 学習結果（ユーザが修正した変換）を反映させる変換エンジンのデコレータ。
///
/// - 入力全体の読みが過去の修正と一致し、エンジンが直す前と同じ結果を出した → その確定文字列に差し替える
///   （「直す前」の記録がない古い学習は、読みが一致すれば常に差し替える）
/// - エンジンが別の結果を出した（左文脈が違うなど）→ エンジンの結果をそのまま使い、
///   候補ウィンドウでは学習結果を2番目に置く
///
/// 読みが一致しなければ何もせず素通しする（文中の一部に当てはめることはしない）。
/// 差し替えるかどうかをエンジンの結果で決めるので、学習済みの読みでもエンジンは毎回呼ぶ。
public final class LearningEngine: ConversionEngine, @unchecked Sendable {

    private let base: any ConversionEngine
    private let dictionaryProvider: @Sendable () -> LearningDictionary

    /// 読み → 直近の第一候補の変換（count 1）で返した結果とエンジンの結果。
    /// 修正を記録するときに「直す前にエンジンが出していた結果」を引くのに使う（`engineResult`）
    private var recentDecisions: [String: (shown: String, engine: String)] = [:]
    private let lock = NSLock()
    private static let maxRecentDecisions = 256

    public init(
        base: any ConversionEngine,
        dictionary: @escaping @Sendable () -> LearningDictionary = {
            LearningStore.shared.current
        }
    ) {
        self.base = base
        self.dictionaryProvider = dictionary
    }

    public func prewarm() async throws {
        try await base.prewarm()
    }

    public func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        let dictionary = dictionaryProvider()
        let entries = dictionary.entries(forReading: reading)
        guard !entries.isEmpty else {
            if candidateCount <= 1 { forgetDecision(forReading: reading) }
            return try await base.convert(
                reading: reading, context: context, candidateCount: candidateCount)
        }

        let baseCandidates: [String]
        do {
            baseCandidates = try await base.convert(
                reading: reading, context: context, candidateCount: candidateCount)
        } catch {
            // エンジンが失敗したら、条件なしで差し替えられる学習結果だけで返す
            // （候補ウィンドウは学習結果が1件でもあれば開く）
            guard !Task.isCancelled else { throw error }
            let fallback = candidateCount <= 1
                ? dictionary.entry(forReading: reading, engineResult: nil).map { [$0.result] } ?? []
                : Self.unique(entries.map(\.result))
            guard !fallback.isEmpty else { throw error }
            InferenceTimer.current?.note(.learning)
            return fallback
        }

        let engineFirst = baseCandidates.first
        let applied = dictionary.entry(forReading: reading, engineResult: engineFirst)

        if candidateCount <= 1 {
            guard let shown = applied?.result ?? engineFirst else { return baseCandidates }
            if let engineFirst { remember(reading: reading, shown: shown, engine: engineFirst) }
            return [shown]
        }

        // 候補ウィンドウ: 差し替える学習結果があれば先頭に、残りの学習結果はその次に置く
        var results = baseCandidates
        if let applied {
            results.removeAll { $0 == applied.result }
            results.insert(applied.result, at: 0)
        }
        let others = Self.unique(entries.map(\.result)).filter { $0 != results.first }
        results.removeAll { others.contains($0) }
        results.insert(contentsOf: others, at: min(1, results.count))
        return results
    }

    /// 直近の第一候補の変換で `reading` に `shown` を返したとき、エンジン（学習を除く）が出していた結果。
    ///
    /// 学習の条件になる読みが無かった・別の結果を返していた場合は nil
    /// （学習が差し替えていないので、表示した結果がそのままエンジンの結果）
    public func engineResult(forReading reading: String, shown: String) -> String? {
        lock.lock()
        defer { lock.unlock() }
        guard let decision = recentDecisions[reading], decision.shown == shown else { return nil }
        return decision.engine
    }

    private func remember(reading: String, shown: String, engine: String) {
        lock.lock()
        defer { lock.unlock() }
        if recentDecisions.count >= Self.maxRecentDecisions { recentDecisions.removeAll() }
        recentDecisions[reading] = (shown, engine)
    }

    private func forgetDecision(forReading reading: String) {
        lock.lock()
        defer { lock.unlock() }
        recentDecisions[reading] = nil
    }

    private static func unique(_ values: [String]) -> [String] {
        var seen: Set<String> = []
        return values.filter { seen.insert($0).inserted }
    }
}
