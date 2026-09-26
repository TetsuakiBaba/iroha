import XCTest
import MLX
import MLXNN
@testable import IrohaTrain
@testable import IrohaCore

/// MLX 実装の T5 が llama.cpp と同じロジットを出すか（アダプタ無し・LoRA 込みの両方）。
/// デコーダの全位置を比べる（因果マスク・相対位置バイアスの取り違えは後ろの位置で露見する）
final class T5ParityTests: XCTestCase {

    override func setUpWithError() throws {
        try TestSupport.prepareMLX()
    }

    /// `ZenzEngine` と同じ形: エンコーダ = U+EE01 まで + </s>、デコーダ = 開始トークン + 出力
    private func sequences(_ tokenizer: VocabTokenizer) -> (source: [Int32], decoder: [Int32]) {
        let source = tokenizer.tokenize("\u{EE02}本日は\u{EE00}キシャガキシャデ\u{EE01}", addSpecial: false) + [tokenizer.eos]
        let decoder = [tokenizer.decoderStartToken] + tokenizer.tokenize("記者が汽車で", addSpecial: false)
        return (source, decoder)
    }

    private func mlxLogits(_ model: T5Model, source: [Int32], decoder: [Int32]) -> [[Float]] {
        let sourceMask = MLXArray.ones([1, source.count])
        let logits = model.decode(MLXArray(decoder, [1, decoder.count]),
                                  memory: model.encode(MLXArray(source, [1, source.count]), sourceMask: sourceMask),
                                  sourceMask: sourceMask)[0].asType(.float32)
        eval(logits)
        let vocab = logits.dim(1)
        let flat = logits.asArray(Float.self)
        return decoder.indices.map { Array(flat[($0 * vocab) ..< (($0 + 1) * vocab)]) }
    }

    private func compare(_ mlx: [[Float]], _ llama: [[Float]], label: String) {
        XCTAssertEqual(mlx.count, llama.count)
        var worst: Float = 0
        for (index, (a, b)) in zip(mlx, llama).enumerated() {
            XCTAssertEqual(TestSupport.argmax(a), TestSupport.argmax(b), "\(label) 位置 \(index) の argmax")
            worst = max(worst, TestSupport.maxAbsDifference(a, b))
        }
        print("\(label) logits max|Δ| = \(worst)")
        XCTAssertLessThan(worst, 0.1)
    }

    func testDecoderStartTokenMatchesMetadata() throws {
        let path = try TestSupport.requireT5Model()
        let tokenizer = try VocabTokenizer(modelPath: path)
        let model = try T5Model(gguf: try GGUFFile(path: path), lora: nil)
        XCTAssertTrue(tokenizer.isEncoderDecoder)
        XCTAssertEqual(tokenizer.decoderStartToken, model.decoderStartToken)
    }

    /// 相対位置バケットが HF transformers の `_relative_position_bucket`（32 バケット・最大距離 128）と一致する
    func testRelativePositionBucket() {
        func bucket(_ relative: Int, _ bidirectional: Bool) -> Int {
            T5Model.relativePositionBucket(key: relative, query: 0, buckets: 32, bidirectional: bidirectional)
        }
        // 双方向: 近距離はそのまま、正（右側）は +16（0 は左側扱い）。8 以上は対数で 15 まで
        XCTAssertEqual([0, 1, 7, 8, 12, 16, 32, 64, 127, 128, 500].map { bucket($0, true) },
                       [0, 17, 23, 24, 25, 26, 28, 30, 31, 31, 31])
        XCTAssertEqual([-1, -7, -8, -16, -128].map { bucket($0, true) }, [1, 7, 8, 10, 15])
        // 因果（デコーダ）: 過去だけを 32 バケットに分ける。未来は 0
        XCTAssertEqual([0, -1, -15, -16, -32, -64, -128, 3].map { bucket($0, false) }, [0, 1, 15, 16, 21, 26, 31, 0])
    }

    /// アダプタ無し: f16 GGUF を MLX に読み、デコーダ全位置のロジットが llama.cpp と一致
    func testLogitsMatchLlamaCpp() throws {
        let path = try TestSupport.requireT5Model()
        let tokenizer = try VocabTokenizer(modelPath: path)
        let (source, decoder) = sequences(tokenizer)
        let model = try T5Model(gguf: try GGUFFile(path: path), lora: nil)
        let llama = try TestSupport.llamaT5Logits(modelPath: path, source: source, decoderTokens: decoder)
        compare(mlxLogits(model, source: source, decoder: decoder), llama, label: "T5")
    }

    /// LoRA 込み: 乱数の B を持つアダプタを書き出し、llama.cpp が適用した結果と MLX 側の LoRA 込みが一致する
    /// （エンコーダ・交差注意・FFN の gate まで全対象層に掛ける。テンソル名の取り違えはここで露見する）
    func testLoraRoundTripMatchesLlamaCpp() throws {
        let path = try TestSupport.requireT5Model()
        let tokenizer = try VocabTokenizer(modelPath: path)
        let (source, decoder) = sequences(tokenizer)
        let spec = LoRASpec(rank: 4, alpha: 8, targets: nil)
        let model = try T5Model(gguf: try GGUFFile(path: path), lora: spec)
        XCTAssertEqual(model.loraLayers.count, 12 * 7 + 2 * 11)

        for (_, layer) in model.loraLayers {
            layer.loraB._updateInternal(MLXRandom.normal(layer.loraB.shape, scale: 0.02))
        }
        let adapterPath = TestSupport.temporaryPath("t5-roundtrip")
        defer { try? FileManager.default.removeItem(atPath: adapterPath) }
        try LoraAdapterWriter.write(to: adapterPath, architecture: "t5", alpha: spec.alpha, pairs: try model.exportLoraPairs())

        let withAdapter = try TestSupport.llamaT5Logits(modelPath: path, adapterPath: adapterPath, source: source,
                                                        decoderTokens: decoder)
        let without = try TestSupport.llamaT5Logits(modelPath: path, source: source, decoderTokens: decoder)
        let effect = zip(withAdapter, without).map { TestSupport.maxAbsDifference($0, $1) }.max() ?? 0
        print("T5 adapter effect = \(effect)")
        XCTAssertGreaterThan(effect, 0.05)
        compare(mlxLogits(model, source: source, decoder: decoder), withAdapter, label: "T5 LoRA")
    }

    /// 凍結後の学習対象は LoRA の A/B だけ（相対位置バイアス・ノルム・埋め込みは動かない）
    func testFreezeLeavesOnlyLoraTrainable() throws {
        let path = try TestSupport.requireT5Model()
        let model = try T5Model(gguf: try GGUFFile(path: path), lora: LoRASpec(rank: 2, alpha: 4, targets: ["attn_q"]))
        model.freezeBase()
        let trainable = model.trainableParameters().flattened()
        XCTAssertEqual(trainable.count, (12 + 2) * 2)
        XCTAssertTrue(trainable.allSatisfy { $0.0.hasSuffix("lora_a") || $0.0.hasSuffix("lora_b") })
    }

    /// 記録の行 → エンコーダ入力 / デコーダ列の分け方が `ZenzEngine`・`train_t5.py` と同じ
    func testEncodeEncoderDecoder() throws {
        let path = try TestSupport.requireT5Model()
        let tokenizer = try VocabTokenizer(modelPath: path)
        let line = "\u{EE02}本日は\u{EE00}キシャ\u{EE01}貴社"
        let examples = try TrainingDataBuilder.encodeEncoderDecoder(
            lines: [line, "\u{EE00}カラ\u{EE01}"], tokenize: { tokenizer.tokenize($0, addSpecial: false) },
            eos: tokenizer.eos, terminator: tokenizer.terminator, decoderStart: tokenizer.decoderStartToken)
        XCTAssertEqual(examples.count, 1, "出力が空の行は捨てる")
        XCTAssertEqual(examples[0].source,
                       tokenizer.tokenize("\u{EE02}本日は\u{EE00}キシャ\u{EE01}", addSpecial: false) + [tokenizer.eos])
        XCTAssertEqual(examples[0].tokens,
                       [tokenizer.decoderStartToken] + tokenizer.tokenize("貴社", addSpecial: false) + [tokenizer.terminator])
        XCTAssertEqual(examples[0].lossFrom, 0)
    }

    /// 小さな合成データで数ステップ回すと損失が下がり、書き出したアダプタを llama.cpp が読める
    func testTrainingReducesLossAndExports() throws {
        let path = try TestSupport.requireT5Model()
        let tokenizer = try VocabTokenizer(modelPath: path)
        let lines = (0..<8).map { _ in "\u{EE02}本日は\u{EE00}キシャ\u{EE01}貴社" }
        let examples = try TrainingDataBuilder.encodeEncoderDecoder(
            lines: lines, tokenize: { tokenizer.tokenize($0, addSpecial: false) }, eos: tokenizer.eos,
            terminator: tokenizer.terminator, decoderStart: tokenizer.decoderStartToken)
        var config = TrainingConfig()
        config.rank = 4
        config.epochs = 4
        config.batchSize = 8
        config.learningRate = 1e-3
        let model = try T5Model(gguf: try GGUFFile(path: path), lora: LoRASpec(config))
        let trainer = LoRATrainer(model: model, config: config, padToken: tokenizer.terminator)
        var losses: [Float] = []
        let last = trainer.train(examples: examples) { losses.append($0.loss) }
        print("T5 losses: \(losses.map { String(format: "%.3f", $0) })")
        XCTAssertEqual(losses.count, 4)
        XCTAssertLessThan(last, losses[0])

        let adapterPath = TestSupport.temporaryPath("t5-trained")
        defer { try? FileManager.default.removeItem(atPath: adapterPath) }
        try LoraAdapterWriter.write(to: adapterPath, architecture: "t5", alpha: config.alpha, pairs: try model.exportLoraPairs())
        _ = try TestSupport.llamaT5Logits(modelPath: path, adapterPath: adapterPath, source: examples[0].source!,
                                          decoderTokens: Array(examples[0].tokens.dropLast()))
    }
}
