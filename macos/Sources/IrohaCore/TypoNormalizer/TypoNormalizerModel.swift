import Foundation

/// 文字単位 Transformer encoder–decoder（3.2M パラメータ）の推論。
///
/// `experiments/typo-normalizer/model.py` の移植。計算の順序はそちらが正で、
/// ずれていないことは `parity.json` の 200 件（ロジット・生成結果・logP）で確かめる
/// （`iroha-cli typo parity`）。**式を触ったら必ず parity を回すこと。**
///
/// 構成は pre-norm（正規化を残差の外ではなく中に入れる形）:
/// ```
/// enc: h = n1(x); x += attn(h, h)            ; x += ffn(n2(x))
/// dec: h = n1(x); x += selfAttn(h, h, causal); h = n2(x); x += crossAttn(h, mem); x += ffn(n3(x))
/// ```
///
/// batch=1 専用。実運用でも検証でも 1 本ずつしか通さないので、パディングは扱わない
/// （`src` に PAD が混じることがない = attention のマスクは因果マスクだけで足りる）。
final class TypoNormalizerModel {

    struct Config {
        let dModel: Int
        let nHead: Int
        let headDim: Int
        let encLayers: Int
        let decLayers: Int
        let dFF: Int
        let maxLength: Int
        let vocabSize: Int
        let outputSize: Int
    }

    private struct Attention {
        let q: LinearWeights
        let k: LinearWeights
        let v: LinearWeights
        let o: LinearWeights
    }

    private struct FeedForward {
        let up: LinearWeights     // Linear(d, dFF)
        let down: LinearWeights   // Linear(dFF, d)
    }

    private struct EncoderLayer {
        let n1: LayerNormWeights
        let n2: LayerNormWeights
        let attention: Attention
        let ffn: FeedForward
    }

    private struct DecoderLayer {
        let n1: LayerNormWeights
        let n2: LayerNormWeights
        let n3: LayerNormWeights
        let selfAttention: Attention
        let crossAttention: Attention
        let ffn: FeedForward
    }

    /// デコーダの KV キャッシュ。self は 1 ステップごとに 1 行追記、cross は最初の 1 回だけ作る
    /// （cross を毎ステップ作り直すのが「遅い移植」の典型。SWIFT-PORT.md §7）
    final class DecoderCache {
        var selfKeys: [Mat] = []
        var selfValues: [Mat] = []
        var crossKeys: [Mat] = []
        var crossValues: [Mat] = []
    }

    let config: Config
    private let weights: TypoNormalizerWeights
    private let embedding: UnsafePointer<Float>
    private let positionSource: UnsafePointer<Float>
    private let positionTarget: UnsafePointer<Float>
    private let encoderLayers: [EncoderLayer]
    private let decoderLayers: [DecoderLayer]
    private let encoderNorm: LayerNormWeights
    private let decoderNorm: LayerNormWeights
    private let head: LinearWeights
    private let embeddingScale: Float

    init(manifest: TypoNormalizerManifest, weights: TypoNormalizerWeights) throws {
        let c = manifest.config
        precondition(c.dModel % c.nHead == 0)
        config = Config(
            dModel: c.dModel, nHead: c.nHead, headDim: c.dModel / c.nHead,
            encLayers: c.encLayers, decLayers: c.decLayers, dFF: c.dFF, maxLength: c.maxLength,
            vocabSize: manifest.vocabSize, outputSize: manifest.outputSize)
        self.weights = weights
        embeddingScale = Float(Double(c.dModel).squareRoot())

        embedding = try weights.tensor("emb.weight", shape: [manifest.vocabSize, c.dModel])
        positionSource = try weights.tensor("pos_src.weight", shape: [c.maxLength, c.dModel])
        positionTarget = try weights.tensor("pos_tgt.weight", shape: [c.maxLength, c.dModel])

        func norm(_ prefix: String) throws -> LayerNormWeights {
            LayerNormWeights(
                weight: try weights.tensor("\(prefix).weight", shape: [c.dModel]),
                bias: try weights.tensor("\(prefix).bias", shape: [c.dModel]),
                dim: c.dModel)
        }
        func linear(_ prefix: String, _ inDim: Int, _ outDim: Int) throws -> LinearWeights {
            LinearWeights(
                weight: try weights.tensor("\(prefix).weight", shape: [outDim, inDim]),
                bias: try weights.tensor("\(prefix).bias", shape: [outDim]),
                inDim: inDim, outDim: outDim)
        }
        func attention(_ prefix: String) throws -> Attention {
            Attention(
                q: try linear("\(prefix).q", c.dModel, c.dModel),
                k: try linear("\(prefix).k", c.dModel, c.dModel),
                v: try linear("\(prefix).v", c.dModel, c.dModel),
                o: try linear("\(prefix).o", c.dModel, c.dModel))
        }
        // FFN は nn.Sequential なので重みの名前は net.0 / net.3（1 は GELU、2 は Dropout）
        func feedForward(_ prefix: String) throws -> FeedForward {
            FeedForward(
                up: try linear("\(prefix).net.0", c.dModel, c.dFF),
                down: try linear("\(prefix).net.3", c.dFF, c.dModel))
        }

        encoderLayers = try (0..<c.encLayers).map { i in
            EncoderLayer(
                n1: try norm("enc.\(i).n1"), n2: try norm("enc.\(i).n2"),
                attention: try attention("enc.\(i).attn"), ffn: try feedForward("enc.\(i).ffn"))
        }
        decoderLayers = try (0..<c.decLayers).map { i in
            DecoderLayer(
                n1: try norm("dec.\(i).n1"), n2: try norm("dec.\(i).n2"), n3: try norm("dec.\(i).n3"),
                selfAttention: try attention("dec.\(i).self_attn"),
                crossAttention: try attention("dec.\(i).cross_attn"),
                ffn: try feedForward("dec.\(i).ffn"))
        }
        encoderNorm = try norm("n_enc")
        decoderNorm = try norm("n_dec")
        head = try linear("head", c.dModel, manifest.outputSize)
    }

    // MARK: - encoder

    /// 読みの id 列（末尾に EOS）→ memory（S×d_model）
    func encode(_ source: [Int]) -> Mat {
        precondition(source.count <= config.maxLength, "読みが位置埋め込みの上限を超えています")
        let x = embed(source, positions: positionSource, offset: 0)
        for layer in encoderLayers {
            let h = Mat(rows: x.rows, cols: config.dModel)
            TypoMath.layerNorm(layer.n1, x: x, out: h)
            let keys = project(layer.attention.k, h)
            let values = project(layer.attention.v, h)
            addInPlace(x, attention(layer.attention, query: h, keys: keys, values: values, causalPrefix: nil))
            let h2 = Mat(rows: x.rows, cols: config.dModel)
            TypoMath.layerNorm(layer.n2, x: x, out: h2)
            addInPlace(x, feedForward(layer.ffn, h2))
        }
        let memory = Mat(rows: x.rows, cols: config.dModel)
        TypoMath.layerNorm(encoderNorm, x: x, out: memory)
        return memory
    }

    // MARK: - decoder

    func makeCache(memory: Mat) -> DecoderCache {
        let cache = DecoderCache()
        for layer in decoderLayers {
            // cross の k/v は memory からの射影なので 1 回で終わり（以後ずっと使い回す）
            cache.crossKeys.append(project(layer.crossAttention.k, memory))
            cache.crossValues.append(project(layer.crossAttention.v, memory))
            cache.selfKeys.append(Mat(rows: 0, cols: config.dModel, capacityRows: config.maxLength))
            cache.selfValues.append(Mat(rows: 0, cols: config.dModel, capacityRows: config.maxLength))
        }
        return cache
    }

    /// `tokens` を 1 回デコードしてロジット（T×n_out）を返す。
    /// `offset` は **キャッシュの長さではなく位置埋め込みの添字**（生成では step と同じ）
    func decode(_ tokens: [Int], cache: DecoderCache, offset: Int) -> Mat {
        precondition(offset + tokens.count <= config.maxLength)
        let x = embed(tokens, positions: positionTarget, offset: offset)
        for (index, layer) in decoderLayers.enumerated() {
            let h = Mat(rows: x.rows, cols: config.dModel)
            TypoMath.layerNorm(layer.n1, x: x, out: h)
            // self: 新しい行ぶんの k/v をキャッシュへ追記してから、貯まっている全部を見る
            let keyCache = cache.selfKeys[index], valueCache = cache.selfValues[index]
            let prefix = keyCache.rows
            TypoMath.linear(layer.selfAttention.k, x: h,
                            into: keyCache.appendRows(h.rows), ldOut: config.dModel)
            TypoMath.linear(layer.selfAttention.v, x: h,
                            into: valueCache.appendRows(h.rows), ldOut: config.dModel)
            addInPlace(x, attention(layer.selfAttention, query: h,
                                    keys: keyCache, values: valueCache, causalPrefix: prefix))
            let h2 = Mat(rows: x.rows, cols: config.dModel)
            TypoMath.layerNorm(layer.n2, x: x, out: h2)
            addInPlace(x, attention(layer.crossAttention, query: h2,
                                    keys: cache.crossKeys[index], values: cache.crossValues[index],
                                    causalPrefix: nil))
            let h3 = Mat(rows: x.rows, cols: config.dModel)
            TypoMath.layerNorm(layer.n3, x: x, out: h3)
            addInPlace(x, feedForward(layer.ffn, h3))
        }
        let normalized = Mat(rows: x.rows, cols: config.dModel)
        TypoMath.layerNorm(decoderNorm, x: x, out: normalized)
        let logits = Mat(rows: x.rows, cols: config.outputSize)
        TypoMath.linear(head, x: normalized, out: logits)
        return logits
    }

    // MARK: - 部品

    private func embed(_ tokens: [Int], positions: UnsafePointer<Float>, offset: Int) -> Mat {
        let x = Mat(rows: tokens.count, cols: config.dModel)
        for (i, token) in tokens.enumerated() {
            let destination = x[i]
            let source = embedding.advanced(by: token * config.dModel)
            let position = positions.advanced(by: (offset + i) * config.dModel)
            for j in 0..<config.dModel {
                destination[j] = source[j] * embeddingScale + position[j]
            }
        }
        return x
    }

    private func project(_ w: LinearWeights, _ x: Mat) -> Mat {
        let out = Mat(rows: x.rows, cols: w.outDim)
        TypoMath.linear(w, x: x, out: out)
        return out
    }

    /// `causalPrefix` に値があれば因果マスク: クエリ行 i はキー行 `causalPrefix + i` までを見る。
    /// nil なら全キーを見る（encoder の self と decoder の cross）
    private func attention(
        _ a: Attention, query: Mat, keys: Mat, values: Mat, causalPrefix: Int?
    ) -> Mat {
        let queries = project(a.q, query)
        let t = queries.rows, s = keys.rows
        let scale = Float(1 / Double(config.headDim).squareRoot())
        let scores = Mat(rows: t, cols: s)
        let context = Mat(rows: t, cols: config.dModel)
        for head in 0..<config.nHead {
            let shift = head * config.headDim
            // scores(t×s) = Q_h(t×dh) · K_h(s×dh)ᵀ / √dh
            TypoMath.gemm(
                m: t, n: s, k: config.headDim, alpha: scale,
                a: queries.data.advanced(by: shift), lda: config.dModel,
                b: keys.data.advanced(by: shift), ldb: config.dModel, transposeB: true,
                beta: 0, c: scores.data, ldc: s)
            if let prefix = causalPrefix, t > 1 {
                for i in 0..<t {
                    let row = scores[i]
                    for j in (prefix + i + 1)..<s { row[j] = -Float.greatestFiniteMagnitude }
                }
            }
            TypoMath.softmaxRows(scores.data, rows: t, cols: s, stride: s)
            // context_h(t×dh) = scores(t×s) · V_h(s×dh)
            TypoMath.gemm(
                m: t, n: config.headDim, k: s, alpha: 1,
                a: scores.data, lda: s,
                b: values.data.advanced(by: shift), ldb: config.dModel, transposeB: false,
                beta: 0, c: context.data.advanced(by: shift), ldc: config.dModel)
        }
        return project(a.o, context)
    }

    private func feedForward(_ f: FeedForward, _ x: Mat) -> Mat {
        let hidden = Mat(rows: x.rows, cols: config.dFF)
        TypoMath.linear(f.up, x: x, out: hidden)
        TypoMath.gelu(hidden)
        let out = Mat(rows: x.rows, cols: config.dModel)
        TypoMath.linear(f.down, x: hidden, out: out)
        return out
    }

    private func addInPlace(_ x: Mat, _ delta: Mat) {
        for i in 0..<(x.rows * x.cols) { x.data[i] += delta.data[i] }
    }
}
