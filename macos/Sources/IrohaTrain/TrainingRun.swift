import Foundation
import MLX
import IrohaCore

/// 追加学習の一連の流れ（データ準備 → 学習前評価 → 学習 → アダプタ書き出し → 学習後評価）。
/// 進捗は `TrainingEvent` で通知する（`iroha-train` は JSON Lines にして標準出力へ流す）
public enum TrainingRun {

    public struct Options: Sendable {
        /// ベースモデル（推論で使っている GGUF。量子化済みでよい）
        public var basePath: String
        /// 出力するアダプタ（GGUF）のパス
        public var outputPath: String
        public var config: TrainingConfig?
        /// held-out を `iroha-cli bench` 用 TSV に書く先（nil なら `<outputPath>.heldout.tsv`）
        public var heldOutTSVPath: String?
        /// 学習前後の held-out 評価を省く（学習ループだけを見たいとき）
        public var skipEvaluation = false
        public var log: ConversionLog = .shared

        public init(basePath: String, outputPath: String, config: TrainingConfig? = nil) {
            self.basePath = basePath
            self.outputPath = outputPath
            self.config = config
        }
    }

    public enum RunError: Error, CustomStringConvertible {
        case notEnoughData(Int)
        case cancelled
        public var description: String {
            switch self {
            case .notEnoughData(let count): return "学習に使える記録が足りません（\(count) 件）"
            case .cancelled: return "中止されました"
            }
        }
    }

    /// `info` 用の集計（MLX には触らない）
    public struct Summary: Sendable, Codable {
        public var totalEntries: Int
        public var usableEntries: Int
        public var trainCount: Int
        public var heldOutCount: Int
        public var architecture: String
        public var supported: Bool
        public var recommendedEpochs: Int
    }

    public static let minimumEntries = 20

    public static func summarize(basePath: String, log: ConversionLog = .shared) throws -> Summary {
        let all = log.fileURLs().flatMap { log.entries(in: $0) }
        let usable = TrainingDataBuilder.dedupe(TrainingDataBuilder.collect(from: log))
        let config = TrainingConfig.recommended(forExampleCount: usable.count)
        let (train, heldOut) = TrainingDataBuilder.split(usable, heldOutFraction: config.heldOutFraction, minHeldOut: config.minHeldOut)
        let architecture = (try? GGUFFile(path: basePath))?.architecture ?? ""
        return Summary(totalEntries: all.count, usableEntries: usable.count, trainCount: train.count, heldOutCount: heldOut.count,
                       architecture: architecture, supported: TrainableModels.supportedArchitectures.contains(architecture),
                       recommendedEpochs: config.epochs)
    }

    public static func run(_ options: Options, emit: @escaping @Sendable (TrainingEvent) -> Void,
                           shouldStop: @escaping @Sendable () -> Bool = { false }) async throws {
        // 1. データ
        let entries = TrainingDataBuilder.dedupe(TrainingDataBuilder.collect(from: options.log))
        guard entries.count >= minimumEntries else { throw RunError.notEnoughData(entries.count) }
        let config = options.config ?? TrainingConfig.recommended(forExampleCount: entries.count)
        let (trainEntries, heldOut) = TrainingDataBuilder.split(entries, heldOutFraction: config.heldOutFraction,
                                                                minHeldOut: config.minHeldOut)
        let tokenizer = try VocabTokenizer(modelPath: options.basePath)
        let lines = TrainingDataBuilder.trainingLines(trainEntries, duplicateEdited: config.duplicateEdited)
        let examples = try TrainingDataBuilder.encode(lines: lines, tokenize: { tokenizer.tokenize($0) },
                                                      eos: tokenizer.terminator, outputTag: tokenizer.outputTagTokens)
        emit(.data(train: trainEntries.count, heldOut: heldOut.count, examples: examples.count))

        let heldOutTSV = options.heldOutTSVPath ?? options.outputPath + ".heldout.tsv"
        if !heldOut.isEmpty {
            try TrainingDataBuilder.heldOutTSV(heldOut).write(toFile: heldOutTSV, atomically: true, encoding: .utf8)
        }

        // 2. 学習前の held-out（本番と同じ量子化ベース・読み制約で変換）
        var before = 0
        if !options.skipEvaluation, !heldOut.isEmpty {
            emit(.stage("evaluate"))
            before = try await TrainingEvaluator.evaluate(entries: heldOut, modelPath: options.basePath, adapterPath: nil).exact
            emit(.eval(phase: "before", exact: before, total: heldOut.count))
        }
        if shouldStop() { throw RunError.cancelled }

        // 3. f16 ベース → MLX へ
        emit(.stage("quantize"))
        let f16 = try ModelRequantizer.ensureF16(basePath: options.basePath)
        emit(.stage("load"))
        let gguf = try GGUFFile(path: f16.path)
        let model = try TrainableModels.load(gguf: gguf, lora: LoRASpec(config))
        guard !model.loraLayers.isEmpty else { throw TrainableLMError.missingTensor("LoRA 対象の層が見つかりません") }

        // 4. 学習
        emit(.stage("train"))
        try trainAndExport(model: model, examples: examples, config: config, padToken: tokenizer.terminator,
                           architecture: gguf.architecture ?? "", outputPath: options.outputPath, emit: emit, shouldStop: shouldStop)

        // 5. 学習後の held-out
        var after = 0
        if !options.skipEvaluation, !heldOut.isEmpty {
            emit(.stage("evaluate"))
            after = try await TrainingEvaluator.evaluate(entries: heldOut, modelPath: options.basePath,
                                                         adapterPath: options.outputPath).exact
            emit(.eval(phase: "after", exact: after, total: heldOut.count))
        }
        emit(.done(adapter: options.outputPath, heldOutTSV: heldOut.isEmpty ? nil : heldOutTSV, before: before, after: after,
                   total: heldOut.count, trainCount: trainEntries.count))
    }

    /// 型消去された `any TrainableLM` を具体型に開いて学習・書き出しする
    private static func trainAndExport(model: any TrainableLM, examples: [TrainingExample], config: TrainingConfig,
                                       padToken: Int32, architecture: String, outputPath: String,
                                       emit: (TrainingEvent) -> Void, shouldStop: () -> Bool) throws {
        func go<M: TrainableLM>(_ model: M) throws {
            let trainer = LoRATrainer(model: model, config: config, padToken: padToken)
            trainer.train(examples: examples, progress: { p in
                emit(.step(epoch: p.epoch, epochs: p.epochs, step: p.step, steps: p.steps, loss: p.loss, elapsed: p.elapsed))
            }, shouldStop: shouldStop)
            if shouldStop() { throw RunError.cancelled }
            emit(.stage("export"))
            try LoraAdapterWriter.write(to: outputPath, architecture: architecture, alpha: config.alpha,
                                        pairs: try model.exportLoraPairs())
        }
        try go(model)
    }
}
