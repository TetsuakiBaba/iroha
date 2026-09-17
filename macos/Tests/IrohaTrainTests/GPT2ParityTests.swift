import XCTest
import MLX
import MLXNN
@testable import IrohaTrain
@testable import IrohaCore

/// MLX 実装の GPT2 が llama.cpp と同じロジットを出すか（アダプタ無し・LoRA 込みの両方）
final class GPT2ParityTests: XCTestCase {

    override func setUpWithError() throws {
        try TestSupport.prepareMLX()
    }

    private func promptTokens(_ tokenizer: VocabTokenizer) -> [Int32] {
        tokenizer.tokenize("\u{EE02}本日は\u{EE00}キシャガキシャデ\u{EE01}記者が汽車")
    }

    /// アダプタ無し: f16 GGUF を MLX に読み、最終位置のロジットが llama.cpp と一致（argmax 一致・差 < 0.1）
    func testLogitsMatchLlamaCpp() throws {
        let f16 = try TestSupport.f16Model()
        let tokenizer = try VocabTokenizer(modelPath: f16)
        let tokens = promptTokens(tokenizer)

        let model = try GPT2Model(gguf: try GGUFFile(path: f16), lora: nil)
        let logits = model(MLXArray(tokens, [1, tokens.count]))[0, tokens.count - 1].asType(.float32)
        eval(logits)
        let mlx = logits.asArray(Float.self)
        let llama = try TestSupport.llamaLogits(modelPath: f16, tokens: tokens)

        XCTAssertEqual(mlx.count, llama.count)
        XCTAssertEqual(TestSupport.argmax(mlx), TestSupport.argmax(llama))
        let difference = TestSupport.maxAbsDifference(mlx, llama)
        print("logits max|Δ| = \(difference)  argmax=\(tokenizer.piece(Int32(TestSupport.argmax(mlx))))")
        XCTAssertLessThan(difference, 0.1)
    }

    /// LoRA 込み: 乱数の B を持つアダプタを書き出し、llama.cpp が適用した結果と MLX 側の LoRA 込みが一致する
    /// （alpha/rank のスケール・A/B の転置・テンソル名の取り違えはここで露見する）
    func testLoraRoundTripMatchesLlamaCpp() throws {
        let f16 = try TestSupport.f16Model()
        let tokenizer = try VocabTokenizer(modelPath: f16)
        let tokens = promptTokens(tokenizer)
        let spec = LoRASpec(rank: 4, alpha: 8, targets: ["attn_qkv", "attn_output", "ffn_up", "ffn_down"])
        let model = try GPT2Model(gguf: try GGUFFile(path: f16), lora: spec)
        XCTAssertEqual(model.loraLayers.count, 12 * 4)

        // B を非ゼロにして LoRA が効く状態にする
        for (_, layer) in model.loraLayers {
            // @ParameterInfo のプロパティは代入不可（Module 側のキャッシュが見ない）。中身を差し替える
            layer.loraB._updateInternal(MLXRandom.normal(layer.loraB.shape, scale: 0.02))
        }
        let adapterPath = TestSupport.temporaryPath("roundtrip")
        defer { try? FileManager.default.removeItem(atPath: adapterPath) }
        try LoraAdapterWriter.write(to: adapterPath, architecture: "gpt2", alpha: spec.alpha, pairs: try model.exportLoraPairs())

        let logits = model(MLXArray(tokens, [1, tokens.count]))[0, tokens.count - 1].asType(.float32)
        eval(logits)
        let mlx = logits.asArray(Float.self)
        let withAdapter = try TestSupport.llamaLogits(modelPath: f16, adapterPath: adapterPath, tokens: tokens)
        let without = try TestSupport.llamaLogits(modelPath: f16, tokens: tokens)

        // アダプタは実際に出力を変えている（一致の確認が「両方ベースのまま」で通っていないことの保証）
        XCTAssertGreaterThan(TestSupport.maxAbsDifference(withAdapter, without), 0.05)
        let difference = TestSupport.maxAbsDifference(mlx, withAdapter)
        print("LoRA logits max|Δ| = \(difference)  (adapter effect = \(TestSupport.maxAbsDifference(withAdapter, without)))")
        XCTAssertEqual(TestSupport.argmax(mlx), TestSupport.argmax(withAdapter))
        XCTAssertLessThan(difference, 0.1)
    }

    /// 凍結後の学習対象は LoRA の A/B だけ
    func testFreezeLeavesOnlyLoraTrainable() throws {
        let f16 = try TestSupport.f16Model()
        let model = try GPT2Model(gguf: try GGUFFile(path: f16), lora: LoRASpec(rank: 2, alpha: 4, targets: ["attn_qkv"]))
        model.freezeBase()
        let trainable = model.trainableParameters().flattened()
        XCTAssertEqual(trainable.count, 12 * 2)
        XCTAssertTrue(trainable.allSatisfy { $0.0.hasSuffix("lora_a") || $0.0.hasSuffix("lora_b") })
    }
}
