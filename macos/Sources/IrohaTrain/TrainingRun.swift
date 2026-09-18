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
        case notEnoughRecords(Int)
        case notEnoughMistakes(screened: Int, mistakes: Int)
        case cancelled
        public var description: String {
            switch self {
            case .notEnoughRecords(let count):
                return "記録が \(count) 件しかありません（\(minimumRecords) 件以上必要）"
            case .notEnoughMistakes(let screened, let mistakes):
                return "いまのモデルは記録 \(screened) 件のうち \(screened - mistakes) 件を既に正解しており、"
                    + "学習できる間違いは \(mistakes) 件だけでした（\(minimumMistakes) 件以上必要）。"
                    + "誤変換を直した確定が溜まってから学習してください"
            case .cancelled: return "中止されました"
            }
        }
    }

    /// `info` 用の集計（モデルを読まない軽い処理。学習できる間違いの数は実際に変換してみないと
    /// 分からないので、ここでは記録の件数と「ユーザが直した確定」の件数だけを返す）
    public struct Summary: Sendable, Codable {
        public var totalEntries: Int
        public var usableEntries: Int
        /// ユーザがエンジンの提示を直した確定（目安。学習対象は実際に変換して選び直す）
        public var corrections: Int
        public var architecture: String
        public var supported: Bool
        public var minimumRecords: Int
        public var minimumMistakes: Int
    }

    /// 学習を始めるのに必要な記録の数
    public static let minimumRecords = 30
    /// 学習に必要な「モデルが間違えた記録」の数（評価用に取り分けたうえで学習に残る数を確保する）
    public static let minimumMistakes = 5

    public static func summarize(basePath: String, log: ConversionLog = .shared) throws -> Summary {
        let all = log.fileURLs().flatMap { log.entries(in: $0) }
        let usable = TrainingDataBuilder.dedupe(TrainingDataBuilder.collect(from: log))
        let corrections = usable.filter(TrainingDataBuilder.isCorrection).count
        let architecture = (try? GGUFFile(path: basePath))?.architecture ?? ""
        return Summary(totalEntries: all.count, usableEntries: usable.count, corrections: corrections,
                       architecture: architecture,
                       supported: TrainableModels.supportedArchitectures.contains(architecture),
                       minimumRecords: minimumRecords, minimumMistakes: minimumMistakes)
    }

    public static func run(_ options: Options, emit: @escaping @Sendable (TrainingEvent) -> Void,
                           shouldStop: @escaping @Sendable () -> Bool = { false }) async throws {
        // 1. 記録を集め、いまのモデルが間違えるものを選り分ける。
        // これが学習前の評価そのものになる（間違えた群は 0 件正解・正解群は全件正解）
        let entries = TrainingDataBuilder.dedupe(TrainingDataBuilder.collect(from: options.log))
        guard entries.count >= minimumRecords else { throw RunError.notEnoughRecords(entries.count) }

        emit(.stage("screen"))
        let screening = try await TrainingScreener.screen(entries: entries, modelPath: options.basePath) { done, total in
            // 流しすぎないよう間引く
            if done % 10 == 0 || done == total { emit(.progress(stage: "screen", done: done, total: total)) }
        }
        let screened = screening.mistakes.count + screening.correct.count
        guard screening.mistakes.count >= minimumMistakes else {
            throw RunError.notEnoughMistakes(screened: screened, mistakes: screening.mistakes.count)
        }
        if shouldStop() { throw RunError.cancelled }

        let config = options.config ?? TrainingConfig.recommended(forMistakeCount: screening.mistakes.count)
        let split = TrainingDataBuilder.stratify(screening, config: config)
        let tokenizer = try VocabTokenizer(modelPath: options.basePath)
        let examples = try TrainingDataBuilder.encode(lines: split.trainLines, tokenize: { tokenizer.tokenize($0) },
                                                      eos: tokenizer.terminator, outputTag: tokenizer.outputTagTokens)
        let summary = TrainingDataSummary(trainLines: examples.count, screened: screened,
                                          mistakes: split.trainMistakes.count, anchors: split.anchors.count,
                                          heldOutMistakes: split.heldOutMistakes.count,
                                          heldOutCorrect: split.heldOutCorrect.count)
        emit(.data(summary))

        // 使った記録は TSV に残す（`iroha-cli bench <tsv>` で同じ数値を再現でき、
        // 何を覚えさせたかも確かめられる）
        let mistakesTSV = writeTSV(split.heldOutMistakes, to: options.outputPath + ".mistakes.tsv")
        let correctTSV = writeTSV(split.heldOutCorrect, to: options.outputPath + ".correct.tsv")
        let trainTSV = writeTSV(split.trainMistakes, to: options.outputPath + ".train.tsv")

        // 2. 学習前の一致数は選り分けの結果そのもの（改めて変換しなくても分かる）
        let before = TrainingScores(
            mistakes: TrainingScore(exact: 0, total: split.heldOutMistakes.count),
            correct: TrainingScore(exact: split.heldOutCorrect.count, total: split.heldOutCorrect.count))
        emit(.eval(phase: "before", scores: before))

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
                           architecture: gguf.architecture ?? "", outputPath: options.outputPath, emit: emit,
                           shouldStop: shouldStop)

        // 5. 学習後の評価（同じ 2 群を、同じ量子化ベース + アダプタで測る）
        var after = TrainingScores()
        if !options.skipEvaluation {
            emit(.stage("evaluate"))
            let result = try await TrainingEvaluator.evaluate(
                mistakes: split.heldOutMistakes, correct: split.heldOutCorrect,
                modelPath: options.basePath, adapterPath: options.outputPath)
            after = TrainingScores(mistakes: result.mistakes.score, correct: result.correct.score)
            emit(.eval(phase: "after", scores: after))
        }
        emit(.done(TrainingResult(adapter: options.outputPath, mistakesTSV: mistakesTSV, correctTSV: correctTSV,
                                  trainTSV: trainTSV, before: before, after: after, data: summary)))
    }

    private static func writeTSV(_ entries: [ConversionLogEntry], to path: String) -> String? {
        guard !entries.isEmpty else { return nil }
        let tsv = TrainingDataBuilder.heldOutTSV(entries)
        guard (try? tsv.write(toFile: path, atomically: true, encoding: .utf8)) != nil else { return nil }
        return path
    }

    /// 型消去された `any TrainableLM` を具体型に開いて学習・書き出しする
    private static func trainAndExport(model: any TrainableLM, examples: [TrainingExample], config: TrainingConfig,
                                       padToken: Int32, architecture: String, outputPath: String,
                                       emit: (TrainingEvent) -> Void, shouldStop: () -> Bool) throws {
        func go<M: TrainableLM>(_ model: M) throws {
            let trainer = LoRATrainer(model: model, config: config, padToken: padToken)
            trainer.train(examples: examples, progress: { emit(.step($0)) }, shouldStop: shouldStop)
            if shouldStop() { throw RunError.cancelled }
            emit(.stage("export"))
            try LoraAdapterWriter.write(to: outputPath, architecture: architecture, alpha: config.alpha,
                                        pairs: try model.exportLoraPairs())
        }
        try go(model)
    }
}
