import Foundation
import MLX
import MLXNN
import MLXRandom
import IrohaCore

/// 凍結した `Linear` に低ランクの差分を足す層: `y = W·x + (alpha/rank)·(x·A)·B`。
/// A:[in, rank]（小さな乱数）、B:[rank, out]（ゼロ初期化なので学習開始時はベースと同じ出力）。
/// llama.cpp 側の適用 `W·x + (alpha/rank)·Bᵀ(Aᵀx)` と同じ式になるよう、書き出し時に A/B を転置する
public final class LoRALinear: Module, UnaryLayer {

    public let base: Linear
    @ParameterInfo(key: "lora_a") public var loraA: MLXArray
    @ParameterInfo(key: "lora_b") public var loraB: MLXArray
    public let rank: Int
    public let alpha: Float

    public var scale: Float { alpha / Float(rank) }
    public var inFeatures: Int { base.shape.1 }
    public var outFeatures: Int { base.shape.0 }

    public init(base: Linear, rank: Int, alpha: Float) {
        self.base = base
        self.rank = rank
        self.alpha = alpha
        let (outFeatures, inFeatures) = base.shape
        let bound = 1 / Float(inFeatures).squareRoot()
        self._loraA = ParameterInfo(wrappedValue: MLXRandom.uniform(low: -bound, high: bound, [inFeatures, rank]), key: "lora_a")
        self._loraB = ParameterInfo(wrappedValue: MLXArray.zeros([rank, outFeatures]), key: "lora_b")
        super.init()
    }

    public func callAsFunction(_ x: MLXArray) -> MLXArray {
        base(x) + scale * matmul(matmul(x, loraA), loraB)
    }

    /// llama.cpp のアダプタ形式（`lora_a`: ne=[in, rank] = 行優先 [rank][in]、`lora_b`: ne=[rank, out] = [out][rank]）
    public func exportPair(baseName: String) throws -> LoraAdapterWriter.Pair {
        let a = loraA.T.asType(.float32)
        let b = loraB.T.asType(.float32)
        eval(a, b)
        return try LoraAdapterWriter.Pair(baseName: baseName, inFeatures: inFeatures, outFeatures: outFeatures, rank: rank,
                                          a: a.asArray(Float.self), b: b.asArray(Float.self))
    }
}
