import Foundation

/// held-out の記録を本番と同じ経路（`ZenzEngine` の貪欲変換・読み制約あり）で変換して一致数を数える。
///
/// 2 群に分けて測るのが要点: 「モデルが間違えていた変換」が出るようになったか（効果）と、
/// 「元から正しく出ていた変換」が壊れていないか（副作用）。
/// 体感では悪化の方が目立つので、後者を必ず並べて見る
public enum TrainingEvaluator {

    public struct GroupResult: Sendable {
        public var exact: Int
        public var total: Int
        /// 不一致だった (読み, 期待, 出力)
        public var mismatches: [(reading: String, expected: String, actual: String)]

        public var score: TrainingScore { TrainingScore(exact: exact, total: total) }
    }

    /// 同じエンジン（＝同じモデル・同じアダプタ）で 2 群を続けて測る
    public static func evaluate(mistakes: [ConversionLogEntry], correct: [ConversionLogEntry],
                                modelPath: String, adapterPath: String?) async throws
        -> (mistakes: GroupResult, correct: GroupResult)
    {
        let engine = ZenzEngine(modelPath: modelPath, adapterPath: adapterPath)
        try await (engine as any ConversionEngine).prewarm()
        return (try await evaluate(entries: mistakes, engine: engine),
                try await evaluate(entries: correct, engine: engine))
    }

    private static func evaluate(entries: [ConversionLogEntry], engine: ZenzEngine) async throws -> GroupResult {
        var result = GroupResult(exact: 0, total: entries.count, mismatches: [])
        for entry in entries {
            let output = try await engine.convert(reading: entry.reading, context: entry.context, candidateCount: 1).first ?? ""
            if output == entry.committed {
                result.exact += 1
            } else {
                result.mismatches.append((entry.reading, entry.committed, output))
            }
        }
        return result
    }
}
