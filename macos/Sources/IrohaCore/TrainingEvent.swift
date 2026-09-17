import Foundation

/// `iroha-train` が標準出力に 1 行 1 JSON で流す進捗。IME 本体（設定画面）がこれを読んで表示する。
/// 両プロセスで同じ型を使うためここ（IrohaCore）に置く
public enum TrainingEvent: Codable, Sendable, Equatable {
    /// 学習データの集計
    case data(train: Int, heldOut: Int, examples: Int)
    /// 段階の切り替わり（"quantize" / "load" / "train" / "export" / "evaluate"）
    case stage(String)
    /// 学習の 1 ステップ
    case step(epoch: Int, epochs: Int, step: Int, steps: Int, loss: Float, elapsed: Double)
    /// held-out の一致数（phase は "before" / "after"）
    case eval(phase: String, exact: Int, total: Int)
    /// 完了。アダプタのパスと学習前後の一致数
    case done(adapter: String, heldOutTSV: String?, before: Int, after: Int, total: Int, trainCount: Int)
    case error(String)

    private enum CodingKeys: String, CodingKey {
        case event, train, heldOut, examples, stage, epoch, epochs, step, steps, loss, elapsed
        case phase, exact, total, adapter, heldOutTSV, before, after, trainCount, message
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        switch try c.decode(String.self, forKey: .event) {
        case "data":
            self = .data(train: try c.decode(Int.self, forKey: .train), heldOut: try c.decode(Int.self, forKey: .heldOut),
                         examples: try c.decode(Int.self, forKey: .examples))
        case "stage":
            self = .stage(try c.decode(String.self, forKey: .stage))
        case "step":
            self = .step(epoch: try c.decode(Int.self, forKey: .epoch), epochs: try c.decode(Int.self, forKey: .epochs),
                         step: try c.decode(Int.self, forKey: .step), steps: try c.decode(Int.self, forKey: .steps),
                         loss: try c.decode(Float.self, forKey: .loss), elapsed: try c.decode(Double.self, forKey: .elapsed))
        case "eval":
            self = .eval(phase: try c.decode(String.self, forKey: .phase), exact: try c.decode(Int.self, forKey: .exact),
                         total: try c.decode(Int.self, forKey: .total))
        case "done":
            self = .done(adapter: try c.decode(String.self, forKey: .adapter),
                         heldOutTSV: try c.decodeIfPresent(String.self, forKey: .heldOutTSV),
                         before: try c.decode(Int.self, forKey: .before), after: try c.decode(Int.self, forKey: .after),
                         total: try c.decode(Int.self, forKey: .total), trainCount: try c.decode(Int.self, forKey: .trainCount))
        case "error":
            self = .error(try c.decode(String.self, forKey: .message))
        case let other:
            throw DecodingError.dataCorruptedError(forKey: .event, in: c, debugDescription: "unknown event \(other)")
        }
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        switch self {
        case .data(let train, let heldOut, let examples):
            try c.encode("data", forKey: .event)
            try c.encode(train, forKey: .train); try c.encode(heldOut, forKey: .heldOut); try c.encode(examples, forKey: .examples)
        case .stage(let stage):
            try c.encode("stage", forKey: .event); try c.encode(stage, forKey: .stage)
        case .step(let epoch, let epochs, let step, let steps, let loss, let elapsed):
            try c.encode("step", forKey: .event)
            try c.encode(epoch, forKey: .epoch); try c.encode(epochs, forKey: .epochs)
            try c.encode(step, forKey: .step); try c.encode(steps, forKey: .steps)
            try c.encode(loss, forKey: .loss); try c.encode(elapsed, forKey: .elapsed)
        case .eval(let phase, let exact, let total):
            try c.encode("eval", forKey: .event)
            try c.encode(phase, forKey: .phase); try c.encode(exact, forKey: .exact); try c.encode(total, forKey: .total)
        case .done(let adapter, let heldOutTSV, let before, let after, let total, let trainCount):
            try c.encode("done", forKey: .event)
            try c.encode(adapter, forKey: .adapter); try c.encodeIfPresent(heldOutTSV, forKey: .heldOutTSV)
            try c.encode(before, forKey: .before); try c.encode(after, forKey: .after)
            try c.encode(total, forKey: .total); try c.encode(trainCount, forKey: .trainCount)
        case .error(let message):
            try c.encode("error", forKey: .event); try c.encode(message, forKey: .message)
        }
    }

    /// 1 行の JSON（改行なし）
    public func jsonLine() -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        guard let data = try? encoder.encode(self), let text = String(data: data, encoding: .utf8) else { return "{}" }
        return text
    }

    public static func parse(line: String) -> TrainingEvent? {
        guard let data = line.data(using: .utf8) else { return nil }
        return try? JSONDecoder().decode(TrainingEvent.self, from: data)
    }
}
