import Foundation

/// 訓練データの内訳
public struct TrainingDataSummary: Codable, Sendable, Equatable {
    /// 学習に使える記録の総数（重複除去後）
    public var records: Int
    /// いまのモデルで変換し直した件数（新しい方から `TrainingScreener.defaultLimit` まで）
    public var screened: Int
    /// そのうち間違えた件数
    public var mistakes: Int
    /// 訓練に使った行数（評価用を除いた記録）
    public var trainLines: Int
    /// 評価用の「間違えた記録」
    public var heldOutMistakes: Int
    /// 評価用の「正解できる記録」
    public var heldOutCorrect: Int

    public init(records: Int, screened: Int, mistakes: Int, trainLines: Int, heldOutMistakes: Int, heldOutCorrect: Int) {
        self.records = records
        self.screened = screened
        self.mistakes = mistakes
        self.trainLines = trainLines
        self.heldOutMistakes = heldOutMistakes
        self.heldOutCorrect = heldOutCorrect
    }
}

public struct TrainingStep: Codable, Sendable, Equatable {
    public var epoch: Int
    public var epochs: Int
    public var step: Int
    public var steps: Int
    public var loss: Float
    public var elapsed: Double

    public init(epoch: Int, epochs: Int, step: Int, steps: Int, loss: Float, elapsed: Double) {
        self.epoch = epoch
        self.epochs = epochs
        self.step = step
        self.steps = steps
        self.loss = loss
        self.elapsed = elapsed
    }
}

/// ある評価群の一致数
public struct TrainingScore: Codable, Sendable, Equatable {
    public var exact: Int
    public var total: Int

    public init(exact: Int = 0, total: Int = 0) {
        self.exact = exact
        self.total = total
    }
}

/// アダプタなし（before）or あり（after）で評価用の記録を変換した一致数（2 群）
public struct TrainingScores: Codable, Sendable, Equatable {
    /// モデルが間違えていた変換（学習で当たるようになってほしいもの）
    public var mistakes: TrainingScore
    /// 元から正しく出ていた変換（壊れていないか）
    public var correct: TrainingScore

    public init(mistakes: TrainingScore = .init(), correct: TrainingScore = .init()) {
        self.mistakes = mistakes
        self.correct = correct
    }
}

public struct TrainingResult: Codable, Sendable, Equatable {
    public var adapter: String
    /// 評価・学習に使った記録の TSV（`iroha-cli bench` で同じ数値を再現できる）
    public var mistakesTSV: String?
    public var correctTSV: String?
    /// 学習に使った記録の TSV（何を覚えさせたかを確かめられる）
    public var trainTSV: String?
    /// 評価用の記録をアダプタなしで変換した一致数
    public var before: TrainingScores
    /// 同じ記録をアダプタありで変換した一致数
    public var after: TrainingScores
    public var data: TrainingDataSummary
    /// 開始から完了までの秒数
    public var elapsed: Double

    public init(adapter: String, mistakesTSV: String?, correctTSV: String?, trainTSV: String?,
                before: TrainingScores, after: TrainingScores, data: TrainingDataSummary, elapsed: Double = 0) {
        self.adapter = adapter
        self.mistakesTSV = mistakesTSV
        self.correctTSV = correctTSV
        self.trainTSV = trainTSV
        self.before = before
        self.after = after
        self.data = data
        self.elapsed = elapsed
    }
}

/// `iroha-train` が標準出力に 1 行 1 JSON で流す進捗。IME 本体（設定画面）がこれを読んで表示する。
/// 両プロセスで同じ型を使うためここ（IrohaCore）に置く
public enum TrainingEvent: Codable, Sendable, Equatable {
    case data(TrainingDataSummary)
    /// 段階の切り替わり（"screen" / "quantize" / "load" / "train" / "export" / "evaluate"）
    case stage(String)
    /// 件数で進む処理（記録の確認など）の進捗
    case progress(stage: String, done: Int, total: Int)
    case step(TrainingStep)
    /// phase は "before"（アダプタなし）/ "after"（アダプタあり）
    case eval(phase: String, scores: TrainingScores)
    case done(TrainingResult)
    case error(String)

    private enum CodingKeys: String, CodingKey {
        case event, data, stage, step, phase, scores, result, message, done, total
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        switch try container.decode(String.self, forKey: .event) {
        case "data": self = .data(try container.decode(TrainingDataSummary.self, forKey: .data))
        case "stage": self = .stage(try container.decode(String.self, forKey: .stage))
        case "progress":
            self = .progress(stage: try container.decode(String.self, forKey: .stage),
                             done: try container.decode(Int.self, forKey: .done),
                             total: try container.decode(Int.self, forKey: .total))
        case "step": self = .step(try container.decode(TrainingStep.self, forKey: .step))
        case "eval":
            self = .eval(phase: try container.decode(String.self, forKey: .phase),
                         scores: try container.decode(TrainingScores.self, forKey: .scores))
        case "done": self = .done(try container.decode(TrainingResult.self, forKey: .result))
        case "error": self = .error(try container.decode(String.self, forKey: .message))
        case let other:
            throw DecodingError.dataCorruptedError(forKey: .event, in: container,
                                                   debugDescription: "unknown event \(other)")
        }
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        switch self {
        case .data(let summary):
            try container.encode("data", forKey: .event)
            try container.encode(summary, forKey: .data)
        case .stage(let stage):
            try container.encode("stage", forKey: .event)
            try container.encode(stage, forKey: .stage)
        case .progress(let stage, let done, let total):
            try container.encode("progress", forKey: .event)
            try container.encode(stage, forKey: .stage)
            try container.encode(done, forKey: .done)
            try container.encode(total, forKey: .total)
        case .step(let step):
            try container.encode("step", forKey: .event)
            try container.encode(step, forKey: .step)
        case .eval(let phase, let scores):
            try container.encode("eval", forKey: .event)
            try container.encode(phase, forKey: .phase)
            try container.encode(scores, forKey: .scores)
        case .done(let result):
            try container.encode("done", forKey: .event)
            try container.encode(result, forKey: .result)
        case .error(let message):
            try container.encode("error", forKey: .event)
            try container.encode(message, forKey: .message)
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
