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
        let adapter: LoRAAdapterMerger?
        let dtype: DType?
        let register: (String, LoRALinear) -> Void

        func linear(_ prefix: String) throws -> UnaryLayer {
            let result = try GGUFWeights.linear(gguf, prefix, lora: lora, adapter: adapter, dtype: dtype)
            if let wrapped = result.lora { register(prefix + ".weight", wrapped) }
            return result.layer
        }

        func optionalLinear(_ prefix: String) throws -> UnaryLayer? {
            gguf.tensor(named: prefix + ".weight") == nil ? nil : try linear(prefix)
        }

        func norm(_ name: String, eps: Float) throws -> RMSNorm {
            RMSNorm(weight: try GGUFWeights.array(gguf, name, dtype: dtype), eps: eps)
        }

        func array(_ name: String) throws -> MLXArray {
            try GGUFWeights.array(gguf, name, dtype: dtype)
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
        let loader = Loader(gguf: gguf, lora: lora, adapter: adapter, dtype: dtype) { registry.register($0, $1) }
        tokenEmbedding = try loader.array("token_embd.weight")
        encoderRelativeBias = try loader.array("enc.blk.0.attn_rel_b.weight")
        decoderRelativeBias = try loader.array("dec.blk.0.attn_rel_b.weight")
        encoderBlocks = try (0..<Int(layers)).map { try EncoderBlock(loader, index: $0, heads: Int(heads), eps: eps) }
        decoderBlocks = try (0..<Int(decoderLayers)).map { try DecoderBlock(loader, index: $0, heads: Int(heads), eps: eps) }
        encoderOutputNorm = try loader.norm("enc.output_norm.weight", eps: eps)
        decoderOutputNorm = try loader.norm("dec.output_norm.weight", eps: eps)
        // output が無いモデルは token_embd と共有（tied。llama.cpp も同じテンソルをそのまま使う）
        if gguf.tensor(named: "output.weight") != nil {
            output = try GGUFWeights.linear(gguf, "output", lora: nil, dtype: dtype).layer
        } else {
            output = Linear(weight: tokenEmbedding)
        }
        try adapter?.verifyAllMerged()
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
        outputLogits(decodeHidden(tokens, memory: memory, sourceMask: sourceMask))
    }

    /// 出力層の重み [V, D] を掛けてロジットにする（`decodeHidden` の後半）
    public func outputLogits(_ hidden: MLXArray) -> MLXArray {
        output(hidden)
    }

    /// `decode` の出力層の手前（最後の正規化の後）[B, T, D]。使う位置だけを選んでから `outputLogits` に
    /// 通せば、語彙ぶんの大きな配列を全位置について作らずに済む（MLX 版エンジンの一括採点）
    public func decodeHidden(_ tokens: MLXArray, memory: MLXArray, sourceMask: MLXArray) -> MLXArray {
        let length = tokens.dim(1)
        let selfBias = positionBias(decoderRelativeBias, queryLength: length, keyLength: length, bidirectional: false)
            + MultiHeadAttention.createAdditiveCausalMask(length)
        let memoryBias = Self.keyPaddingBias(sourceMask)
        var h = tokenEmbedding[tokens]
        for block in decoderBlocks {
            h = block(h, selfBias: selfBias, memory: memory, memoryBias: memoryBias)
        }
        return decoderOutputNorm(h)
    }

    // MARK: - 推論（KV キャッシュで 1 トークンずつ）

    /// 1 トークンずつのデコードの状態。交差注意のキー・バリューはエンコード後に 1 回だけ作り、
    /// 自己注意のキー・バリューは `capacity` 位置ぶんの配列で持ち、位置が進むたびにその位置だけを
    /// 差し替えた新しい配列にする（MLXArray の添字代入はオブジェクトをその場で書き換えるので使わない。
    /// 分岐した状態どうしが同じ配列を指していても壊れない）。
    /// まだ書いていない位置は `decoderSelfBias` の因果マスクで隠れる。
    ///
    /// 配列の形を位置によらず一定にしているのは MLX のメモリの使い回しのため（解放したメモリは
    /// ほぼ同じ大きさの要求にしか使われないので、毎ステップ形が変わると確保し直しになり遅い。
    /// llama.cpp が KV の長さを 256 単位に切り上げているのと同じ考え方）。
    /// MLXArray は不変なので、分岐（n-best）はこの構造体のコピーで済む
    public struct DecoderState {
        let crossKeys: [MLXArray]
        let crossValues: [MLXArray]
        /// エンコーダ入力のパディング位置を隠す加算マスク [1, 1, 1, S]（パディングが無ければ nil）
        let crossBias: MLXArray?
        var keys: [MLXArray]
        var values: [MLXArray]
        /// 位置の番号 [1, 1, capacity, 1]（書き込む位置を選ぶのに使う）
        let slots: MLXArray
        /// 入れられるトークンの数（`decoderSelfBias` の長さもこれ以上にする）
        public let capacity: Int
        /// 次に入れるトークンの位置
        public internal(set) var position = 0

        /// 評価（`eval`）しておくべき配列（キャッシュをグラフのまま持ち越さないため）
        public var arrays: [MLXArray] { keys + values }
    }

    private func splitHeads(_ a: MLXArray, heads: Int) -> MLXArray {
        a.reshaped(a.dim(0), a.dim(1), heads, -1).transposed(0, 2, 1, 3)
    }

    /// `Attention.callAsFunction` と同じ式（1/√d のスケールなし）を、分けたヘッドで行う
    private func attend(_ q: MLXArray, _ k: MLXArray, _ v: MLXArray, bias: MLXArray?) -> MLXArray {
        var scores = matmul(q, k.transposed(0, 1, 3, 2))
        if let bias { scores = scores + bias }
        let out = matmul(softmax(scores, axis: -1, precise: true), v)
        return out.transposed(0, 2, 1, 3).reshaped(q.dim(0), q.dim(2), -1)
    }

    /// エンコーダ出力 [1, S, D] からデコードを始める（交差注意のキー・バリューを作り、自己注意の KV を確保する）。
    /// `sourceMask`（[1, S]、1 = 実トークン）を渡すと、エンコーダ入力のパディング位置を交差注意から隠す
    public func startDecoding(memory: MLXArray, sourceMask: MLXArray? = nil, capacity: Int) -> DecoderState {
        let heads = decoderBlocks[0].attn.heads
        let crossKeys = decoderBlocks.map { splitHeads($0.cross.k(memory), heads: heads) }
        let headDim = crossKeys[0].dim(3)
        let empty = MLXArray.zeros([1, heads, capacity, headDim], dtype: memory.dtype)
        return DecoderState(
            crossKeys: crossKeys,
            crossValues: decoderBlocks.map { splitHeads($0.cross.v(memory), heads: heads) },
            crossBias: sourceMask.map { Self.keyPaddingBias($0).asType(memory.dtype) },
            keys: Array(repeating: empty, count: decoderBlocks.count),
            values: Array(repeating: empty, count: decoderBlocks.count),
            slots: MLXArray(Array(0 ..< Int32(capacity)), [1, 1, capacity, 1]),
            capacity: capacity)
    }

    /// デコーダの自己注意のバイアス [1, H, length, length]（相対位置バイアス + 因果マスク）。
    /// `decodeStep` に渡す。長さは `DecoderState.capacity` 以上にする（作り置きしてよい）
    public func decoderSelfBias(length: Int) -> MLXArray {
        let bias = positionBias(decoderRelativeBias, queryLength: length, keyLength: length, bidirectional: false)
        return bias + MultiHeadAttention.createAdditiveCausalMask(length).asType(bias.dtype)
    }

    /// デコーダに 1 トークン入れ、次のトークンのロジット [V] を返す。`decode` を最後の 1 位置に絞ったもの
    /// （過去の位置は `state` のキャッシュを使う）。`selfBias` は `decoderSelfBias` で作ったもの
    public func decodeStep(_ token: Int32, state: inout DecoderState, selfBias: MLXArray) -> MLXArray {
        let t = state.position
        precondition(t < state.capacity, "DecoderState の容量（\(state.capacity)）を超えた")
        let bias = selfBias[0..., 0..., t ..< t + 1, 0 ..< state.capacity]
        let heads = decoderBlocks[0].attn.heads
        let slot = state.slots .== MLXArray(Int32(t))
        var h = tokenEmbedding[MLXArray([token], [1, 1])]
        for (i, block) in decoderBlocks.enumerated() {
            let normed = block.attnNorm(h)
            let q = splitHeads(block.attn.q(normed), heads: heads)
            state.keys[i] = which(slot, splitHeads(block.attn.k(normed), heads: heads), state.keys[i])
            state.values[i] = which(slot, splitHeads(block.attn.v(normed), heads: heads), state.values[i])
            h = h + block.attn.o(attend(q, state.keys[i], state.values[i], bias: bias))
            let crossQuery = splitHeads(block.cross.q(block.crossNorm(h)), heads: heads)
            h = h + block.cross.o(attend(crossQuery, state.crossKeys[i], state.crossValues[i], bias: state.crossBias))
            h = h + block.ffn(block.ffnNorm(h))
        }
        state.position += 1
        return output(decoderOutputNorm(h))[0, 0]
    }

    public func logits(_ batch: TrainingBatch) -> MLXArray {
        guard let source = batch.source, let sourceMask = batch.sourceMask else {
            preconditionFailure("エンコーダ・デコーダ型の学習にはエンコーダの入力（TrainingExample.source）が要る")
        }
        return decode(batch.inputs, memory: encode(source, sourceMask: sourceMask), sourceMask: sourceMask)
    }
}
