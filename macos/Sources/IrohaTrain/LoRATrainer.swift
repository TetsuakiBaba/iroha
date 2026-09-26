import Foundation
import MLX
import MLXNN
import MLXOptimizers
import IrohaCore

/// LoRA の学習ループ。損失は出力部（U+EE01 の次から EOS まで）だけに掛ける（`training/train.py` と同じ。
/// エンコーダ・デコーダ型ではデコーダ側がちょうど出力部なので、`training/t5/train_t5.py` と同じになる）
public final class LoRATrainer<Model: TrainableLM> {

    public let model: Model
    public let config: TrainingConfig
    /// パディングに使うトークン（EOS。マスクで損失からは外れる）
    public let padToken: Int32

    public init(model: Model, config: TrainingConfig, padToken: Int32) {
        self.model = model
        self.config = config
        self.padToken = padToken
    }

    /// 1 バッチ分の配列。`inputs = tokens[:, :-1]`、`targets = tokens[:, 1:]`、
    /// `mask[t] = 1` は `lossFrom ≤ t < len-1`（= 出力トークンと EOS を予測する位置）。
    /// 例が `source` を持つ（エンコーダ・デコーダ型）なら、それも右パディングして有効位置のマスクを付ける
    static func makeBatch(_ examples: [TrainingExample], padToken: Int32) -> TrainingBatch {
        let length = examples.map(\.tokens.count).max() ?? 0
        var inputs: [Int32] = [], targets: [Int32] = [], mask: [Float] = []
        inputs.reserveCapacity(examples.count * (length - 1))
        for example in examples {
            let padded = example.tokens + [Int32](repeating: padToken, count: length - example.tokens.count)
            inputs += padded.dropLast()
            targets += padded.dropFirst()
            for t in 0..<(length - 1) {
                mask.append(t >= example.lossFrom && t + 1 < example.tokens.count ? 1 : 0)
            }
        }
        let shape = [examples.count, length - 1]
        guard examples.contains(where: { $0.source != nil }) else {
            return TrainingBatch(inputs: MLXArray(inputs, shape), targets: MLXArray(targets, shape), mask: MLXArray(mask, shape))
        }
        let sourceLength = examples.map { $0.source?.count ?? 0 }.max() ?? 0
        var source: [Int32] = [], sourceMask: [Float] = []
        for example in examples {
            let tokens = example.source ?? []
            source += tokens + [Int32](repeating: padToken, count: sourceLength - tokens.count)
            sourceMask += [Float](repeating: 1, count: tokens.count) + [Float](repeating: 0, count: sourceLength - tokens.count)
        }
        let sourceShape = [examples.count, sourceLength]
        return TrainingBatch(inputs: MLXArray(inputs, shape), targets: MLXArray(targets, shape), mask: MLXArray(mask, shape),
                             source: MLXArray(source, sourceShape), sourceMask: MLXArray(sourceMask, sourceShape))
    }

    static func maskedLoss(model: Model, batch: TrainingBatch) -> MLXArray {
        let logits = model.logits(batch)
        let losses = crossEntropy(logits: logits, targets: batch.targets, reduction: .none)
        return (losses * batch.mask).sum() / maximum(batch.mask.sum(), MLXArray(1.0 as Float))
    }

    /// 学習する。`progress` は各ステップの後、`epochEnded` は各エポックの後（エポック番号と平均損失）に呼ばれ、
    /// `shouldStop` が真を返したら途中で止める。戻り値は最後のエポックの平均損失
    @discardableResult
    public func train(examples: [TrainingExample], progress: (TrainingStep) -> Void = { _ in },
                      epochEnded: (Int, Float) throws -> Void = { _, _ in },
                      shouldStop: () -> Bool = { false }) rethrows -> Float {
        model.freezeBase()
        let trainable = model.trainableParameters().flattened().count
        precondition(trainable == model.loraLayers.count * 2, "学習対象が LoRA だけになっていません: \(trainable)")

        let optimizer = AdamW(learningRate: config.learningRate, weightDecay: 0)
        let lossAndGrad = valueAndGrad(model: model) { (model: Model, arrays: [MLXArray]) -> [MLXArray] in
            [Self.maskedLoss(model: model, batch: TrainingBatch(arrays: arrays))]
        }

        // 似た長さをまとめてパディングを減らす（バッチ内は長さ順、バッチの順序は毎エポックシャッフル）
        let sorted = examples.sorted { $0.tokens.count < $1.tokens.count }
        let batches = stride(from: 0, to: sorted.count, by: config.batchSize).map {
            Array(sorted[$0 ..< min($0 + config.batchSize, sorted.count)])
        }
        let totalSteps = batches.count * config.epochs
        var generator = SystemRandomNumberGenerator()
        let start = Date()
        var step = 0
        var lastEpochLoss: Float = 0

        for epoch in 1...max(config.epochs, 1) {
            var epochLoss: Float = 0
            for batch in batches.shuffled(using: &generator) {
                if shouldStop() { return lastEpochLoss }
                let (values, gradients) = lossAndGrad(model, Self.makeBatch(batch, padToken: padToken).arrays)
                optimizer.update(model: model, gradients: gradients)
                eval(model, optimizer)
                let loss = values[0].item(Float.self)
                epochLoss += loss
                step += 1
                progress(TrainingStep(epoch: epoch, epochs: config.epochs, step: step, steps: totalSteps, loss: loss,
                                      elapsed: Date().timeIntervalSince(start)))
            }
            lastEpochLoss = epochLoss / Float(max(batches.count, 1))
            try epochEnded(epoch, lastEpochLoss)
        }
        return lastEpochLoss
    }
}
