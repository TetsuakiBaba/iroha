import Foundation
import MLX
import IrohaCore

/// 追加学習の一連の流れ。
///
/// 1. 記録をいまのモデル（アダプタなし）で変換し直し、間違いを見つける
/// 2. 間違いの一部と正解の一部を評価用に取り分け、残り全件を訓練データにする
/// 3. LoRA を学習してアダプタを書き出す
/// 4. 評価用の記録をアダプタなし／ありで変換して結果を並べる
///
/// 進捗は `TrainingEvent` で通知する（`iroha-train` は JSON Lines にして標準出力へ流す）
public enum TrainingRun {

    public struct Options: Sendable {
        /// ベースモデル（推論で使っている GGUF。量子化済みでよい）
        public var basePath: String
        /// 出力するアダプタ（GGUF）のパス
        public var outputPath: String
        public var config: TrainingConfig?
        /// 学習後のアダプタなし／あり評価を省く（学習ループだけを見たいとき）
        public var skipEvaluation = false
        /// N エポックごとの途中のアダプタも `<出力>.ep<N>.gguf` に書き、学習後にそれぞれ評価する
        /// （エポック数と効果の関係を見る開発用。学習率は一定なので、途中の N エポック目は N エポックで学習したものと同じ）
        public var checkpointEvery: Int?
        public var log: ConversionLog = .shared

        public init(basePath: String, outputPath: String, config: TrainingConfig? = nil) {
            self.basePath = basePath
            self.outputPath = outputPath
            self.config = config
        }
    }

    public enum RunError: Error, CustomStringConvertible {
        case notEnoughRecords(Int)
        case cancelled
        public var description: String {
            switch self {
            case .notEnoughRecords(let count):
                return "記録が \(count) 件しかありません（\(minimumRecords) 件以上必要）"
            case .cancelled: return "中止されました"
            }
        }
    }

    /// `info` 用の集計（モデルを読まない軽い処理。記録の件数と対応可否だけを返す）
    public struct Summary: Sendable, Codable {
        public var totalEntries: Int
        public var usableEntries: Int
        public var architecture: String
        public var supported: Bool
        public var minimumRecords: Int
    }

    /// 学習を始めるのに必要な記録の数
    public static let minimumRecords = 30

    public static func summarize(basePath: String, log: ConversionLog = .shared) throws -> Summary {
        let all = log.fileURLs().flatMap { log.entries(in: $0) }
        let usable = TrainingDataBuilder.dedupe(TrainingDataBuilder.collect(from: log))
        let architecture = (try? GGUFFile(path: basePath))?.architecture ?? ""
        return Summary(totalEntries: all.count, usableEntries: usable.count, architecture: architecture,
                       supported: TrainableModels.supportedArchitectures.contains(architecture),
                       minimumRecords: minimumRecords)
    }

    public static func run(_ options: Options, emit: @escaping @Sendable (TrainingEvent) -> Void,
                           shouldStop: @escaping @Sendable () -> Bool = { false }) async throws {
        let started = Date()
        // 1. 記録を集め、いまのモデル（アダプタなし）で変換し直して間違いを見つける
        let entries = TrainingDataBuilder.dedupe(TrainingDataBuilder.collect(from: options.log))
        guard entries.count >= minimumRecords else { throw RunError.notEnoughRecords(entries.count) }

        emit(.stage("screen"))
        let screening = try await TrainingScreener.screen(entries: entries, modelPath: options.basePath) { done, total in
            // 流しすぎないよう間引く
            if done % 10 == 0 || done == total { emit(.progress(stage: "screen", done: done, total: total)) }
        }
        if shouldStop() { throw RunError.cancelled }

        // 2. 評価用を取り分け、残り全件を訓練データにする
        let config = options.config ?? TrainingConfig()
        let split = TrainingDataBuilder.stratify(entries: entries, screening: screening)
        let tokenizer = try VocabTokenizer(modelPath: options.basePath)
        let examples = tokenizer.isEncoderDecoder
            ? try TrainingDataBuilder.encodeEncoderDecoder(
                lines: split.trainLines, tokenize: { tokenizer.tokenize($0, addSpecial: false) }, eos: tokenizer.eos,
                terminator: tokenizer.terminator, decoderStart: tokenizer.decoderStartToken)
            : try TrainingDataBuilder.encode(lines: split.trainLines, tokenize: { tokenizer.tokenize($0) },
                                             eos: tokenizer.terminator, outputTag: tokenizer.outputTagTokens)
        let summary = TrainingDataSummary(records: entries.count,
                                          screened: screening.mistakes.count + screening.correct.count,
                                          mistakes: screening.mistakes.count, trainLines: examples.count,
                                          heldOutMistakes: split.heldOutMistakes.count,
                                          heldOutCorrect: split.heldOutCorrect.count)
        emit(.data(summary))

        // 使った記録は TSV に残す（`iroha-cli bench <tsv>` で同じ数値を再現でき、
        // 何を覚えさせたかも確かめられる）
        let mistakesTSV = writeTSV(split.heldOutMistakes, to: options.outputPath + ".mistakes.tsv")
        let correctTSV = writeTSV(split.heldOutCorrect, to: options.outputPath + ".correct.tsv")
        let trainTSV = writeTSV(split.train, to: options.outputPath + ".train.tsv")

        // 3. f16 ベース → MLX へ → 学習 → アダプタ書き出し
        emit(.stage("quantize"))
        let f16 = try ModelRequantizer.ensureF16(basePath: options.basePath)
        emit(.stage("load"))
        let gguf = try GGUFFile(path: f16.path)
        let model = try TrainableModels.load(gguf: gguf, lora: LoRASpec(config))
        guard !model.loraLayers.isEmpty else { throw TrainableLMError.missingTensor("LoRA 対象の層が見つかりません") }

        emit(.stage("train"))
        let checkpoints = try trainAndExport(model: model, examples: examples, config: config,
                                             padToken: tokenizer.terminator, architecture: gguf.architecture ?? "",
                                             outputPath: options.outputPath, checkpointEvery: options.checkpointEvery,
                                             emit: emit, shouldStop: shouldStop)

        // 4. 評価用の記録を、同じ量子化ベースでアダプタなし／ありの両方で変換して並べる。
        // 「なし」は変換し直しの結果（間違い 0 件・正解は全件）と同じになるはずだが、
        // 見せる数字は実際に測ったものにする
        var before = TrainingScores()
        var after = TrainingScores()
        if !options.skipEvaluation {
            emit(.stage("evaluate"))
            let base = try await TrainingEvaluator.evaluate(
                mistakes: split.heldOutMistakes, correct: split.heldOutCorrect,
                modelPath: options.basePath, adapterPath: nil)
            before = TrainingScores(mistakes: base.mistakes.score, correct: base.correct.score)
            emit(.eval(phase: "before", scores: before))
            for (epoch, path) in checkpoints {
                let result = try await TrainingEvaluator.evaluate(
                    mistakes: split.heldOutMistakes, correct: split.heldOutCorrect,
                    modelPath: options.basePath, adapterPath: path)
                emit(.eval(phase: "ep\(epoch)", scores: TrainingScores(mistakes: result.mistakes.score,
                                                                       correct: result.correct.score)))
            }
            let adapted = try await TrainingEvaluator.evaluate(
                mistakes: split.heldOutMistakes, correct: split.heldOutCorrect,
                modelPath: options.basePath, adapterPath: options.outputPath)
            after = TrainingScores(mistakes: adapted.mistakes.score, correct: adapted.correct.score)
            emit(.eval(phase: "after", scores: after))
        }
        emit(.done(TrainingResult(adapter: options.outputPath, mistakesTSV: mistakesTSV, correctTSV: correctTSV,
                                  trainTSV: trainTSV, before: before, after: after, data: summary,
                                  elapsed: Date().timeIntervalSince(started))))
    }

    private static func writeTSV(_ entries: [ConversionLogEntry], to path: String) -> String? {
        guard !entries.isEmpty else { return nil }
        let tsv = TrainingDataBuilder.heldOutTSV(entries)
        guard (try? tsv.write(toFile: path, atomically: true, encoding: .utf8)) != nil else { return nil }
        return path
    }

    /// 型消去された `any TrainableLM` を具体型に開いて学習・書き出しする。
    /// 戻り値は途中で書いたアダプタ（エポック, パス）。最後のエポックの分は `outputPath` なので含めない
    private static func trainAndExport(model: any TrainableLM, examples: [TrainingExample], config: TrainingConfig,
                                       padToken: Int32, architecture: String, outputPath: String, checkpointEvery: Int?,
                                       emit: (TrainingEvent) -> Void, shouldStop: () -> Bool) throws -> [(Int, String)] {
        func go<M: TrainableLM>(_ model: M) throws -> [(Int, String)] {
            var checkpoints: [(Int, String)] = []
            let trainer = LoRATrainer(model: model, config: config, padToken: padToken)
            try trainer.train(examples: examples, progress: { emit(.step($0)) }, epochEnded: { epoch, _ in
                guard let every = checkpointEvery, every > 0, epoch % every == 0, epoch < config.epochs else { return }
                let path = outputPath.replacingOccurrences(of: ".gguf", with: "") + ".ep\(epoch).gguf"
                try LoraAdapterWriter.write(to: path, architecture: architecture, alpha: config.alpha,
                                            pairs: try model.exportLoraPairs())
                checkpoints.append((epoch, path))
            }, shouldStop: shouldStop)
            if shouldStop() { throw RunError.cancelled }
            emit(.stage("export"))
            try LoraAdapterWriter.write(to: outputPath, architecture: architecture, alpha: config.alpha,
                                        pairs: try model.exportLoraPairs())
            return checkpoints
        }
        return try go(model)
    }
}
