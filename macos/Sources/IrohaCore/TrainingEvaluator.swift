import Foundation

/// held-out の記録を本番と同じ経路（`ZenzEngine` の貪欲変換・読み制約あり）で変換して一致数を数える。
/// 学習前（アダプタ無し）と学習後（アダプタ有り）を同じ条件で比べるために使う
public enum TrainingEvaluator {

    public struct Result: Sendable, Equatable {
        public var exact: Int
        public var total: Int
        /// 不一致だった (読み, 期待, 出力)
        public var mismatches: [(reading: String, expected: String, actual: String)]

        public static func == (lhs: Result, rhs: Result) -> Bool {
            lhs.exact == rhs.exact && lhs.total == rhs.total
        }
    }

    public static func evaluate(entries: [ConversionLogEntry], modelPath: String, adapterPath: String?) async throws -> Result {
        let engine = ZenzEngine(modelPath: modelPath, adapterPath: adapterPath)
        try await (engine as any ConversionEngine).prewarm()
        var result = Result(exact: 0, total: entries.count, mismatches: [])
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
