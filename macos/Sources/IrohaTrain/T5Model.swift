import Foundation
import MLX
import MLXNN
import MLXFast
import IrohaCore

/// T5（training/t5/ で学習する文字単位のエンコーダ・デコーダ）。llama.cpp の `src/models/t5.cpp` と同じ計算:
///
/// - エンコーダ: `h = tok_embd[src]` → 各層 { RMSNorm → 自己注意（相対位置バイアス・双方向）→ 残差;
///   RMSNorm → FFN → 残差 } → RMSNorm
/// - デコーダ: `h = tok_embd[dec]` → 各層 { RMSNorm → 自己注意（相対位置バイアス・因果）→ 残差;
///   RMSNorm → 交差注意（位置バイアスなし）→ 残差; RMSNorm → FFN → 残差 } → RMSNorm → output
///
/// 注意は `softmax(q·kᵀ + bias)·v`（T5 は 1/√d のスケールを掛けない）。相対位置バイアスは各層が持たなければ
/// 0 層目のものを使う。FFN は `ffn_gate` があれば `down(gelu(gate·x) ⊙ up·x)`（gated-GELU、tanh 近似）、
/// 無ければ `down(relu(up·x))`。出力のロジットのスケールは GGUF 変換時に `output.weight` へ焼き込み済み
/// （`training/t5/fix_gguf_scale.py`）なので、ここでは掛けない
public final class T5Model: Module, TrainableLM {

    public static let architecture = "t5"
    /// ブロック内の線形層すべて（GPT-2 の既定と同じ範囲: 注意の q/k/v/o と FFN）
    public static let defaultLoRATargets = [
        "attn_q", "attn_k", "attn_v", "attn_o",
        "cross_attn_q", "cross_attn_k", "cross_attn_v", "cross_attn_o",
        "ffn_gate", "ffn_up", "ffn_down",
    ]

    /// 重みだけの RMSNorm（`MLXFast.rmsNorm`）
    final class RMSNorm: Module {
        let weight: MLXArray
        let eps: Float
        init(weight: MLXArray, eps: Float) {
            self.weight = weight
            self.eps = eps
            super.init()
        }
        func callAsFunction(_ x: MLXArray) -> MLXArray {
            MLXFast.rmsNorm(x, weight: weight, eps: eps)
        }
    }

    /// 重みを読み、LoRA 対象なら差し替えて登録する
    struct Loader {
        let gguf: GGUFFile
        let lora: LoRASpec?
        let register: (String, LoRALinear) -> Void

        func linear(_ prefix: String) throws -> UnaryLayer {
            let result = try GGUFWeights.linear(gguf, prefix, lora: lora)
            if let wrapped = result.lora { register(prefix + ".weight", wrapped) }
            return result.layer
        }

        func optionalLinear(_ prefix: String) throws -> UnaryLayer? {
            gguf.tensor(named: prefix + ".weight") == nil ? nil : try linear(prefix)
        }

        func norm(_ name: String, eps: Float) throws -> RMSNorm {
            RMSNorm(weight: try GGUFWeights.array(gguf, name), eps: eps)
        }
    }

    final class Attention: Module {
        let q: UnaryLayer
        let k: UnaryLayer
        let v: UnaryLayer
        let o: UnaryLayer
        let heads: Int

        /// `prefix` は "enc.blk.0.attn" / "dec.blk.0.cross_attn" など
        init(_ loader: Loader, prefix: String, heads: Int) throws {
            q = try loader.linear(prefix + "_q")
            k = try loader.linear(prefix + "_k")
            v = try loader.linear(prefix + "_v")
            o = try loader.linear(prefix + "_o")
            self.heads = heads
            super.init()
        }

        /// `x` [B, Tq, D] が `memory` [B, Tk, D] を見る。`bias` は [B or 1, H or 1, Tq, Tk] に広がる加算マスク
        func callAsFunction(_ x: MLXArray, memory: MLXArray, bias: MLXArray) -> MLXArray {
            let batch = x.dim(0), queryLength = x.dim(1), keyLength = memory.dim(1)
            func splitHeads(_ a: MLXArray, _ length: Int) -> MLXArray {
                a.reshaped(batch, length, heads, -1).transposed(0, 2, 1, 3)
            }
            let queries = splitHeads(q(x), queryLength)
            let keys = splitHeads(k(memory), keyLength)
            let values = splitHeads(v(memory), keyLength)
            let scores = matmul(queries, keys.transposed(0, 1, 3, 2)) + bias
            let attended = matmul(softmax(scores, axis: -1, precise: true), values)
            return o(attended.transposed(0, 2, 1, 3).reshaped(batch, queryLength, -1))
        }
    }

    final class FeedForward: Module {
        let gate: UnaryLayer?
        let up: UnaryLayer
        let down: UnaryLayer

        init(_ loader: Loader, prefix: String) throws {
            gate = try loader.optionalLinear(prefix + "ffn_gate")
            up = try loader.linear(prefix + "ffn_up")
            down = try loader.linear(prefix + "ffn_down")
            super.init()
        }

        func callAsFunction(_ x: MLXArray) -> MLXArray {
            if let gate { return down(geluApproximate(gate(x)) * up(x)) }
            return down(relu(up(x)))
        }
    }

    final class EncoderBlock: Module {
        let attnNorm: RMSNorm
        let attn: Attention
        let ffnNorm: RMSNorm
        let ffn: FeedForward

        init(_ loader: Loader, index: Int, heads: Int, eps: Float) throws {
            let prefix = "enc.blk.\(index)."
            attnNorm = try loader.norm(prefix + "attn_norm.weight", eps: eps)
            attn = try Attention(loader, prefix: prefix + "attn", heads: heads)
            ffnNorm = try loader.norm(prefix + "ffn_norm.weight", eps: eps)
            ffn = try FeedForward(loader, prefix: prefix)
            super.init()
        }

        func callAsFunction(_ x: MLXArray, bias: MLXArray) -> MLXArray {
            let normed = attnNorm(x)
            let h = x + attn(normed, memory: normed, bias: bias)
            return h + ffn(ffnNorm(h))
        }
    }

    final class DecoderBlock: Module {
        let attnNorm: RMSNorm
        let attn: Attention
        let crossNorm: RMSNorm
        let cross: Attention
        let ffnNorm: RMSNorm
        let ffn: FeedForward

        init(_ loader: Loader, index: Int, heads: Int, eps: Float) throws {
            let prefix = "dec.blk.\(index)."
            attnNorm = try loader.norm(prefix + "attn_norm.weight", eps: eps)
            attn = try Attention(loader, prefix: prefix + "attn", heads: heads)
            crossNorm = try loader.norm(prefix + "cross_attn_norm.weight", eps: eps)
            cross = try Attention(loader, prefix: prefix + "cross_attn", heads: heads)
            ffnNorm = try loader.norm(prefix + "ffn_norm.weight", eps: eps)
            ffn = try FeedForward(loader, prefix: prefix)
            super.init()
        }

        func callAsFunction(_ x: MLXArray, selfBias: MLXArray, memory: MLXArray, memoryBias: MLXArray) -> MLXArray {
            let normed = attnNorm(x)
            var h = x + attn(normed, memory: normed, bias: selfBias)
            h = h + cross(crossNorm(h), memory: memory, bias: memoryBias)
            return h + ffn(ffnNorm(h))
        }
    }

    let tokenEmbedding: MLXArray
    /// 相対位置バイアス [バケット, ヘッド]（0 層目のもの。T5 は全層で共有する）
    let encoderRelativeBias: MLXArray
    let decoderRelativeBias: MLXArray
    let encoderBlocks: [EncoderBlock]
    let decoderBlocks: [DecoderBlock]
    let encoderOutputNorm: RMSNorm
    let decoderOutputNorm: RMSNorm
    let output: UnaryLayer
    let relativeBuckets: Int
    public let contextLength: Int
    /// デコーダの開始トークン（`t5.decoder_start_token_id`、無ければ 0 = pad。学習側の設定と同じ）
    public let decoderStartToken: Int32
    public let lora = LoRARegistry()

    public init(gguf: GGUFFile, lora: LoRASpec?) throws {
        let lora = lora?.resolved(defaults: Self.defaultLoRATargets)
        guard gguf.architecture == Self.architecture else {
            throw TrainableLMError.unsupportedArchitecture(gguf.architecture ?? "(不明)")
        }
        guard let layers = gguf.uint32("t5.block_count") else { throw TrainableLMError.missingMetadata("t5.block_count") }
        guard let heads = gguf.uint32("t5.attention.head_count") else {
            throw TrainableLMError.missingMetadata("t5.attention.head_count")
        }
        guard let buckets = gguf.uint32("t5.attention.relative_buckets_count") else {
            throw TrainableLMError.missingMetadata("t5.attention.relative_buckets_count")
        }
        let decoderLayers = gguf.uint32("t5.decoder_block_count") ?? layers
        let eps = gguf.float("t5.attention.layer_norm_rms_epsilon") ?? 1e-6
        relativeBuckets = Int(buckets)
        contextLength = Int(gguf.uint32("t5.context_length") ?? 512)
        decoderStartToken = Int32(gguf.uint32("t5.decoder_start_token_id") ?? 0)

        let registry = self.lora
        let loader = Loader(gguf: gguf, lora: lora) { registry.register($0, $1) }
        tokenEmbedding = try GGUFWeights.array(gguf, "token_embd.weight")
        encoderRelativeBias = try GGUFWeights.array(gguf, "enc.blk.0.attn_rel_b.weight")
        decoderRelativeBias = try GGUFWeights.array(gguf, "dec.blk.0.attn_rel_b.weight")
        encoderBlocks = try (0..<Int(layers)).map { try EncoderBlock(loader, index: $0, heads: Int(heads), eps: eps) }
        decoderBlocks = try (0..<Int(decoderLayers)).map { try DecoderBlock(loader, index: $0, heads: Int(heads), eps: eps) }
        encoderOutputNorm = try loader.norm("enc.output_norm.weight", eps: eps)
        decoderOutputNorm = try loader.norm("dec.output_norm.weight", eps: eps)
        // output が無いモデルは token_embd と共有（tied。llama.cpp も同じテンソルをそのまま使う）
        if gguf.tensor(named: "output.weight") != nil {
            output = try GGUFWeights.linear(gguf, "output", lora: nil).layer
        } else {
            output = Linear(weight: tokenEmbedding)
        }
        super.init()
    }

    // MARK: - 相対位置バイアス

    /// llama.cpp の `llama_relative_position_bucket` と同じ式（float/double の混ぜ方まで合わせる。
    /// HF transformers の `_relative_position_bucket` と同じ結果）。`key - query` の相対位置を分類する
    static func relativePositionBucket(key: Int, query: Int, buckets: Int, bidirectional: Bool) -> Int {
        let maxDistance = 128
        var count = buckets
        if bidirectional { count >>= 1 }
        let maxExact = count >> 1
        var position = key - query
        var bucket = 0
        if bidirectional {
            if position > 0 { bucket += count }
            position = abs(position)
        } else {
            position = -min(position, 0)
        }
        if position < maxExact { return bucket + position }
        let scaled = Double(logf(Float(Double(position) / Double(maxExact))) * Float(count - maxExact))
            / log(Double(maxDistance) / Double(maxExact))
        let large = Int(floorf(Float(Double(maxExact) + scaled)))
        return bucket + min(large, count - 1)
    }

    /// [1, H, Tq, Tk] のバイアス
    func positionBias(_ table: MLXArray, queryLength: Int, keyLength: Int, bidirectional: Bool) -> MLXArray {
        var indices: [Int32] = []
        indices.reserveCapacity(queryLength * keyLength)
        for query in 0..<queryLength {
            for key in 0..<keyLength {
                indices.append(Int32(Self.relativePositionBucket(key: key, query: query, buckets: relativeBuckets,
                                                                 bidirectional: bidirectional)))
            }
        }
        let bias = table[MLXArray(indices, [queryLength, keyLength])]  // [Tq, Tk, H]
        return bias.transposed(2, 0, 1).expandedDimensions(axis: 0)
    }

    /// 有効位置マスク [B, S]（1/0）→ キー側の加算マスク [B, 1, 1, S]
    static func keyPaddingBias(_ mask: MLXArray) -> MLXArray {
        ((mask - 1) * 1e9).expandedDimensions(axes: [1, 2])
    }

    // MARK: - 順伝播

    /// エンコーダ出力 [B, S, D]。`sourceMask` は有効位置（1/0）[B, S]
    public func encode(_ source: MLXArray, sourceMask: MLXArray) -> MLXArray {
        let length = source.dim(1)
        let bias = positionBias(encoderRelativeBias, queryLength: length, keyLength: length, bidirectional: true)
            + Self.keyPaddingBias(sourceMask)
        var h = tokenEmbedding[source]
        for block in encoderBlocks {
            h = block(h, bias: bias)
        }
        return encoderOutputNorm(h)
    }

    /// デコーダ入力 [B, T] の各位置のロジット [B, T, V]
    public func decode(_ tokens: MLXArray, memory: MLXArray, sourceMask: MLXArray) -> MLXArray {
        let length = tokens.dim(1)
        let selfBias = positionBias(decoderRelativeBias, queryLength: length, keyLength: length, bidirectional: false)
            + MultiHeadAttention.createAdditiveCausalMask(length)
        let memoryBias = Self.keyPaddingBias(sourceMask)
        var h = tokenEmbedding[tokens]
        for block in decoderBlocks {
            h = block(h, selfBias: selfBias, memory: memory, memoryBias: memoryBias)
        }
        return output(decoderOutputNorm(h))
    }

    public func logits(_ batch: TrainingBatch) -> MLXArray {
        guard let source = batch.source, let sourceMask = batch.sourceMask else {
            preconditionFailure("エンコーダ・デコーダ型の学習にはエンコーダの入力（TrainingExample.source）が要る")
        }
        return decode(batch.inputs, memory: encode(source, sourceMask: sourceMask), sourceMask: sourceMask)
    }
}
