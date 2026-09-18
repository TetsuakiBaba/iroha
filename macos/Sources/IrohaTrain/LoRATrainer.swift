import Foundation
import MLX
import MLXNN
import MLXOptimizers
import IrohaCore

/// LoRA の学習ループ。損失は出力部（U+EE01 の次から EOS まで）だけに掛ける（`training/train.py` と同じ）
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
    /// `mask[t] = 1` は `lossFrom ≤ t < len-1`（= 出力トークンと EOS を予測する位置）
    static func makeBatch(_ examples: [TrainingExample], padToken: Int32) -> (inputs: MLXArray, targets: MLXArray, mask: MLXArray) {
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
        return (MLXArray(inputs, shape), MLXArray(targets, shape), MLXArray(mask, shape))
    }

    static func maskedLoss(model: Model, inputs: MLXArray, targets: MLXArray, mask: MLXArray) -> MLXArray {
        let logits = model(inputs)
        let losses = crossEntropy(logits: logits, targets: targets, reduction: .none)
        return (losses * mask).sum() / maximum(mask.sum(), MLXArray(1.0 as Float))
    }

    /// 学習する。`progress` は各ステップの後に呼ばれ、`shouldStop` が真を返したら途中で止める。
    /// 戻り値は最後のエポックの平均損失
    @discardableResult
    public func train(examples: [TrainingExample], progress: (TrainingStep) -> Void = { _ in },
                      shouldStop: () -> Bool = { false }) -> Float {
        model.freezeBase()
        let trainable = model.trainableParameters().flattened().count
        precondition(trainable == model.loraLayers.count * 2, "学習対象が LoRA だけになっていません: \(trainable)")

        let optimizer = AdamW(learningRate: config.learningRate, weightDecay: 0)
        let lossAndGrad = valueAndGrad(model: model) { (model: Model, arrays: [MLXArray]) -> [MLXArray] in
            [Self.maskedLoss(model: model, inputs: arrays[0], targets: arrays[1], mask: arrays[2])]
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
                let (inputs, targets, mask) = Self.makeBatch(batch, padToken: padToken)
                let (values, gradients) = lossAndGrad(model, [inputs, targets, mask])
                optimizer.update(model: model, gradients: gradients)
                eval(model, optimizer)
                let loss = values[0].item(Float.self)
                epochLoss += loss
                step += 1
                progress(TrainingStep(epoch: epoch, epochs: config.epochs, step: step, steps: totalSteps, loss: loss,
                                      elapsed: Date().timeIntervalSince(start)))
            }
            lastEpochLoss = epochLoss / Float(max(batches.count, 1))
        }
        return lastEpochLoss
    }
}
