import Foundation
import MLX
import MLXNN
import MLXFast
import IrohaCore

/// GPT-2（zenz-v3 系）。llama.cpp の `src/models/gpt2.cpp` と同じ計算:
/// `h = tok_embd[t] + pos_embd[i]` → 各層 { LN → 融合 QKV（q,k,v 順）→ 因果注意 → 出力射影 → 残差;
/// LN → up → GELU(tanh 近似) → down → 残差 } → LN → output
///
/// 推論（`MLXConversionEngine`）では KV キャッシュで 1 トークンずつ生成する（`startDecoding` / `prefill` /
/// `decodeStep`。作りは `T5Model.DecoderState` と同じ）
public final class GPT2Model: Module, TrainableLM {

    public static let architecture = "gpt2"
    public static let defaultLoRATargets = ["attn_qkv", "attn_output", "ffn_up", "ffn_down"]

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

        init(gguf: GGUFFile, index: Int, heads: Int, eps: Float, lora: LoRASpec?, adapter: LoRAAdapterMerger?,
             dtype: DType?, register: (String, LoRALinear) -> Void) throws {
            let prefix = "blk.\(index)."
            self.heads = heads
            attnNorm = AffineLayerNorm(weight: try GGUFWeights.array(gguf, prefix + "attn_norm.weight", dtype: dtype),
                                       bias: try GGUFWeights.optionalArray(gguf, prefix + "attn_norm.bias", dtype: dtype),
                                       eps: eps)
            ffnNorm = AffineLayerNorm(weight: try GGUFWeights.array(gguf, prefix + "ffn_norm.weight", dtype: dtype),
                                      bias: try GGUFWeights.optionalArray(gguf, prefix + "ffn_norm.bias", dtype: dtype),
                                      eps: eps)
            var built: [(String, (layer: UnaryLayer, lora: LoRALinear?))] = []
            for name in ["attn_qkv", "attn_output", "ffn_up", "ffn_down"] {
                let result = try GGUFWeights.linear(gguf, prefix + name, lora: lora, adapter: adapter, dtype: dtype)
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
            let (queries, keys, values) = projections(x)
            return finish(x, attend(queries, keys, values, mask: mask))
        }

        /// 注意の q, k, v（ヘッドに分けた [B, H, T, d]）
        func projections(_ x: MLXArray) -> (MLXArray, MLXArray, MLXArray) {
            let batch = x.dim(0), length = x.dim(1)
            let qkv = attnQKV(attnNorm(x)).split(parts: 3, axis: -1)
            func splitHeads(_ a: MLXArray) -> MLXArray {
                a.reshaped(batch, length, heads, -1).transposed(0, 2, 1, 3)
            }
            return (splitHeads(qkv[0]), splitHeads(qkv[1]), splitHeads(qkv[2]))
        }

        /// 因果注意。`mask` は加算マスク（[T, Tk] か [1, 1, T, Tk]）。結果はヘッドをまとめた [B, T, D]
        func attend(_ queries: MLXArray, _ keys: MLXArray, _ values: MLXArray, mask: MLXArray) -> MLXArray {
            let attended = MLXFast.scaledDotProductAttention(
                queries: queries, keys: keys, values: values,
                scale: 1 / Float(queries.dim(3)).squareRoot(), mask: mask)
            return attended.transposed(0, 2, 1, 3).reshaped(queries.dim(0), queries.dim(2), -1)
        }

        /// 注意の結果から、出力射影 → 残差 → FFN → 残差
        func finish(_ x: MLXArray, _ attended: MLXArray) -> MLXArray {
            let h = x + attnOutput(attended)
            return h + ffnDown(geluApproximate(ffnUp(ffnNorm(h))))
        }
    }

    let tokenEmbedding: MLXArray
    let positionEmbedding: MLXArray
    let blocks: [Block]
    let outputNorm: AffineLayerNorm
    let output: UnaryLayer
    public let contextLength: Int
    public let lora = LoRARegistry()

    public convenience init(gguf: GGUFFile, lora: LoRASpec?) throws {
        try self.init(gguf: gguf, lora: lora, adapter: nil, dtype: nil)
    }

    /// 推論用の読み込み（`MLXConversionEngine` が使う）。`adapter` を渡すと、その LoRA アダプタ（llama.cpp 形式の
    /// GGUF）をベースの重みに足し込む。`dtype` を渡すと重みをその型で持つ（nil なら学習と同じ f32）
    public init(gguf: GGUFFile, lora: LoRASpec?, adapter: LoRAAdapterMerger?, dtype: DType?) throws {
        let lora = lora?.resolved(defaults: Self.defaultLoRATargets)
        guard gguf.architecture == Self.architecture else {
            throw TrainableLMError.unsupportedArchitecture(gguf.architecture ?? "(不明)")
        }
        guard let layers = gguf.uint32("gpt2.block_count") else { throw TrainableLMError.missingMetadata("gpt2.block_count") }
        guard let heads = gguf.uint32("gpt2.attention.head_count") else {
            throw TrainableLMError.missingMetadata("gpt2.attention.head_count")
        }
        let eps = gguf.float("gpt2.attention.layer_norm_epsilon") ?? 1e-5
        contextLength = Int(gguf.uint32("gpt2.context_length") ?? 1024)

        tokenEmbedding = try GGUFWeights.array(gguf, "token_embd.weight", dtype: dtype)
        positionEmbedding = try GGUFWeights.array(gguf, "position_embd.weight", dtype: dtype)
        let registry = self.lora
        blocks = try (0..<Int(layers)).map { index in
            try Block(gguf: gguf, index: index, heads: Int(heads), eps: eps, lora: lora, adapter: adapter,
                      dtype: dtype) { registry.register($0, $1) }
        }
        outputNorm = AffineLayerNorm(weight: try GGUFWeights.array(gguf, "output_norm.weight", dtype: dtype),
                                     bias: try GGUFWeights.optionalArray(gguf, "output_norm.bias", dtype: dtype), eps: eps)
        // output が無いモデルは token_embd と共有（tied）
        if gguf.tensor(named: "output.weight") != nil {
            output = try GGUFWeights.linear(gguf, "output", lora: nil, dtype: dtype).layer
        } else {
            output = Linear(weight: tokenEmbedding)
        }
        try adapter?.verifyAllMerged()
        super.init()
    }

    public func logits(_ batch: TrainingBatch) -> MLXArray {
        self(batch.inputs)
    }

    /// トークン列 [B, T] → ロジット [B, T, V]
    public func callAsFunction(_ tokens: MLXArray) -> MLXArray {
        output(hidden(tokens))
    }

    /// 出力層の手前（最後の正規化の後）[B, T, D]。使う位置だけを選んでから `outputLogits` に通せば、
    /// 語彙ぶんの大きな配列を全位置について作らずに済む（MLX 版エンジンの一括採点）
    public func hidden(_ tokens: MLXArray) -> MLXArray {
        let length = tokens.dim(1)
        var h = tokenEmbedding[tokens] + positionEmbedding[0 ..< length]
        let mask = MultiHeadAttention.createAdditiveCausalMask(length).asType(h.dtype)
        for block in blocks {
            h = block(h, mask: mask)
        }
        return outputNorm(h)
    }

    /// 出力層の重み [V, D] を掛けてロジットにする（`hidden` の後）
    public func outputLogits(_ hidden: MLXArray) -> MLXArray {
        output(hidden)
    }

    // MARK: - 推論（KV キャッシュで 1 トークンずつ）

    /// 1 トークンずつの生成の状態。キー・バリューは `capacity` 位置ぶんの配列で持ち、位置が進むたびに
    /// その位置だけを差し替えた新しい配列にする（考え方と理由は `T5Model.DecoderState` と同じ。
    /// MLXArray は不変なので、分岐（n-best）はこの構造体のコピーで済む）。
    /// まだ書いていない位置は、`decodeStep` が位置ごとに作る因果マスクで隠れる
    public struct DecoderState {
        var keys: [MLXArray]
        var values: [MLXArray]
        /// 位置の番号 [1, 1, capacity, 1]（書き込む位置を選ぶのに使う）
        let slots: MLXArray
        /// 位置の番号 [1, 1, 1, capacity]（見てよいキーを選ぶのに使う）
        let keyPositions: MLXArray
        /// 入れられるトークンの数
        public let capacity: Int
        /// 次に入れるトークンの位置
        public internal(set) var position = 0

        /// 評価（`eval`）しておくべき配列（キャッシュをグラフのまま持ち越さないため）
        public var arrays: [MLXArray] { keys + values }
    }

    public func startDecoding(capacity: Int) -> DecoderState {
        let heads = blocks[0].heads
        let empty = MLXArray.zeros([1, heads, capacity, tokenEmbedding.dim(1) / heads], dtype: tokenEmbedding.dtype)
        let positions = Array(0 ..< Int32(capacity))
        return DecoderState(
            keys: Array(repeating: empty, count: blocks.count),
            values: Array(repeating: empty, count: blocks.count),
            slots: MLXArray(positions, [1, 1, capacity, 1]),
            keyPositions: MLXArray(positions, [1, 1, 1, capacity]),
            capacity: capacity)
    }

    /// プロンプト `tokens` をまとめて入れ、次のトークンのロジット [V] を返す（`state` は空であること）。
    /// 形をそろえるため後ろを 0 で埋めて `paddedLength` 位置ぶん計算する。埋めた位置の KV もキャッシュに入るが、
    /// 因果的なのでプロンプトの値には影響せず、続く `decodeStep` が位置 `tokens.count` から上書きする
    /// （それまでは因果マスクで隠れる）
    public func prefill(_ tokens: [Int32], paddedLength: Int, state: inout DecoderState) -> MLXArray {
        let count = tokens.count
        precondition(state.position == 0 && count > 0 && count <= paddedLength && paddedLength <= state.capacity
                     && paddedLength <= contextLength)
        let input = MLXArray(tokens + [Int32](repeating: 0, count: paddedLength - count), [1, paddedLength])
        var h = tokenEmbedding[input] + positionEmbedding[0 ..< paddedLength]
        let mask = MultiHeadAttention.createAdditiveCausalMask(paddedLength).asType(h.dtype)
        for (i, block) in blocks.enumerated() {
            let (queries, keys, values) = block.projections(h)
            h = block.finish(h, block.attend(queries, keys, values, mask: mask))
            if paddedLength < state.capacity {
                let rest = state.keys[i][0..., 0..., paddedLength..., 0...]
                state.keys[i] = concatenated([keys, rest], axis: 2)
                state.values[i] = concatenated([values, state.values[i][0..., 0..., paddedLength..., 0...]], axis: 2)
            } else {
                state.keys[i] = keys
                state.values[i] = values
            }
        }
        state.position = count
        return output(outputNorm(h[0..., (count - 1) ..< count]))[0, 0]
    }

    /// 1 トークン入れて、次のトークンのロジット [V] を返す（過去の位置は `state` のキャッシュを使う）
    public func decodeStep(_ token: Int32, state: inout DecoderState) -> MLXArray {
        let t = state.position
        precondition(t < state.capacity && t < contextLength, "位置 \(t) が容量・文脈長を超えた")
        let current = MLXArray(Int32(t))
        let slot = state.slots .== current
        let mask = (state.keyPositions .> current).asType(tokenEmbedding.dtype) * -1e9
        var h = tokenEmbedding[MLXArray([token], [1, 1])] + positionEmbedding[t ..< t + 1]
        for (i, block) in blocks.enumerated() {
            let (queries, keys, values) = block.projections(h)
            state.keys[i] = which(slot, keys, state.keys[i])
            state.values[i] = which(slot, values, state.values[i])
            h = block.finish(h, block.attend(queries, state.keys[i], state.values[i], mask: mask))
        }
        state.position += 1
        return output(outputNorm(h))[0, 0]
    }
}
