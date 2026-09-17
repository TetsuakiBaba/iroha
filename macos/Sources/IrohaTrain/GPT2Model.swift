import Foundation
import MLX
import MLXNN
import MLXFast
import IrohaCore

/// GPT-2（zenz-v3 系）。llama.cpp の `src/models/gpt2.cpp` と同じ計算:
/// `h = tok_embd[t] + pos_embd[i]` → 各層 { LN → 融合 QKV（q,k,v 順）→ 因果注意 → 出力射影 → 残差;
/// LN → up → GELU(tanh 近似) → down → 残差 } → LN → output
public final class GPT2Model: Module, TrainableLM {

    public static let architecture = "gpt2"

    /// 重み・バイアス付き LayerNorm（`MLXFast.layerNorm`）
    final class AffineLayerNorm: Module {
        let weight: MLXArray
        let bias: MLXArray?
        let eps: Float
        init(weight: MLXArray, bias: MLXArray?, eps: Float) {
            self.weight = weight
            self.bias = bias
            self.eps = eps
            super.init()
        }
        func callAsFunction(_ x: MLXArray) -> MLXArray {
            MLXFast.layerNorm(x, weight: weight, bias: bias, eps: eps)
        }
    }

    final class Block: Module {
        let attnNorm: AffineLayerNorm
        let attnQKV: UnaryLayer
        let attnOutput: UnaryLayer
        let ffnNorm: AffineLayerNorm
        let ffnUp: UnaryLayer
        let ffnDown: UnaryLayer
        let heads: Int

        init(gguf: GGUFFile, index: Int, heads: Int, eps: Float, lora: LoRASpec?, register: (String, LoRALinear) -> Void) throws {
            let prefix = "blk.\(index)."
            self.heads = heads
            attnNorm = AffineLayerNorm(weight: try GGUFWeights.array(gguf, prefix + "attn_norm.weight"),
                                       bias: try GGUFWeights.optionalArray(gguf, prefix + "attn_norm.bias"), eps: eps)
            ffnNorm = AffineLayerNorm(weight: try GGUFWeights.array(gguf, prefix + "ffn_norm.weight"),
                                      bias: try GGUFWeights.optionalArray(gguf, prefix + "ffn_norm.bias"), eps: eps)
            var built: [(String, (layer: UnaryLayer, lora: LoRALinear?))] = []
            for name in ["attn_qkv", "attn_output", "ffn_up", "ffn_down"] {
                let result = try GGUFWeights.linear(gguf, prefix + name, lora: lora)
                if let lora = result.lora { register(prefix + name + ".weight", lora) }
                built.append((name, result))
            }
            attnQKV = built[0].1.layer
            attnOutput = built[1].1.layer
            ffnUp = built[2].1.layer
            ffnDown = built[3].1.layer
            super.init()
        }

        func callAsFunction(_ x: MLXArray, mask: MLXArray) -> MLXArray {
            let batch = x.dim(0), length = x.dim(1), width = x.dim(2)
            let headDim = width / heads
            let qkv = attnQKV(attnNorm(x)).split(parts: 3, axis: -1)
            func splitHeads(_ a: MLXArray) -> MLXArray {
                a.reshaped(batch, length, heads, headDim).transposed(0, 2, 1, 3)
            }
            let attended = MLXFast.scaledDotProductAttention(
                queries: splitHeads(qkv[0]), keys: splitHeads(qkv[1]), values: splitHeads(qkv[2]),
                scale: 1 / Float(headDim).squareRoot(), mask: mask)
            var h = x + attnOutput(attended.transposed(0, 2, 1, 3).reshaped(batch, length, width))
            h = h + ffnDown(geluApproximate(ffnUp(ffnNorm(h))))
            return h
        }
    }

    let tokenEmbedding: MLXArray
    let positionEmbedding: MLXArray
    let blocks: [Block]
    let outputNorm: AffineLayerNorm
    let output: UnaryLayer
    public let contextLength: Int
    public let lora = LoRARegistry()

    public init(gguf: GGUFFile, lora: LoRASpec?) throws {
        guard gguf.architecture == Self.architecture else {
            throw TrainableLMError.unsupportedArchitecture(gguf.architecture ?? "(不明)")
        }
        guard let layers = gguf.uint32("gpt2.block_count") else { throw TrainableLMError.missingMetadata("gpt2.block_count") }
        guard let heads = gguf.uint32("gpt2.attention.head_count") else {
            throw TrainableLMError.missingMetadata("gpt2.attention.head_count")
        }
        let eps = gguf.float("gpt2.attention.layer_norm_epsilon") ?? 1e-5
        contextLength = Int(gguf.uint32("gpt2.context_length") ?? 1024)

        tokenEmbedding = try GGUFWeights.array(gguf, "token_embd.weight")
        positionEmbedding = try GGUFWeights.array(gguf, "position_embd.weight")
        let registry = self.lora
        blocks = try (0..<Int(layers)).map { index in
            try Block(gguf: gguf, index: index, heads: Int(heads), eps: eps, lora: lora) { registry.register($0, $1) }
        }
        outputNorm = AffineLayerNorm(weight: try GGUFWeights.array(gguf, "output_norm.weight"),
                                     bias: try GGUFWeights.optionalArray(gguf, "output_norm.bias"), eps: eps)
        // output が無いモデルは token_embd と共有（tied）
        if gguf.tensor(named: "output.weight") != nil {
            output = try GGUFWeights.linear(gguf, "output", lora: nil).layer
        } else {
            output = Linear(weight: tokenEmbedding)
        }
        super.init()
    }

    public func callAsFunction(_ tokens: MLXArray) -> MLXArray {
        let length = tokens.dim(1)
        var h = tokenEmbedding[tokens] + positionEmbedding[0 ..< length]
        let mask = MultiHeadAttention.createAdditiveCausalMask(length)
        for block in blocks {
            h = block(h, mask: mask)
        }
        return output(outputNorm(h))
    }
}
