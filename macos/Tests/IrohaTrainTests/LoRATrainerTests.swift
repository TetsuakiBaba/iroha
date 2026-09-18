import XCTest
import MLX
@testable import IrohaTrain
@testable import IrohaCore

final class LoRATrainerTests: XCTestCase {

    override func setUpWithError() throws {
        try TestSupport.prepareMLX()
    }

    /// バッチ: 右パディング・targets は 1 つずらし・マスクは出力部と EOS の位置だけ
    func testMakeBatch() {
        let a = TrainingExample(line: "a", tokens: [10, 11, 99, 20, 21, 2], lossFrom: 2)  // 99 = タグ
        let b = TrainingExample(line: "b", tokens: [10, 99, 30, 2], lossFrom: 1)
        let (inputs, targets, mask) = LoRATrainer<GPT2Model>.makeBatch([a, b], padToken: 2)
        XCTAssertEqual(inputs.shape, [2, 5])
        XCTAssertEqual(inputs.asArray(Int32.self), [10, 11, 99, 20, 21, 10, 99, 30, 2, 2])
        XCTAssertEqual(targets.asArray(Int32.self), [11, 99, 20, 21, 2, 99, 30, 2, 2, 2])
        XCTAssertEqual(mask.asArray(Float.self), [0, 0, 1, 1, 1, 0, 1, 1, 0, 0])
    }

    /// 小さな合成データで数ステップ回すと損失が下がり、書き出したアダプタを llama.cpp が読める。
    /// バッチが毎エポックシャッフルされるので、1 種類の例だけ（＝毎ステップ同じバッチ）で単調性を見る
    func testTrainingReducesLossAndExports() throws {
        let f16 = try TestSupport.f16Model()
        let tokenizer = try VocabTokenizer(modelPath: f16)
        // モデルが出さない表記をわざと確定した記録のつもり
        let lines = (0..<8).map { _ in "\u{EE02}本日は\u{EE00}キシャ\u{EE01}貴社" }
        let examples = try TrainingDataBuilder.encode(lines: lines, tokenize: { tokenizer.tokenize($0) },
                                                      eos: tokenizer.terminator, outputTag: tokenizer.outputTagTokens)
        var config = TrainingConfig()
        config.rank = 4
        config.epochs = 4
        config.batchSize = 8
        config.learningRate = 1e-3
        let model = try GPT2Model(gguf: try GGUFFile(path: f16), lora: LoRASpec(config))
        let trainer = LoRATrainer(model: model, config: config, padToken: tokenizer.terminator)
        var losses: [Float] = []
        let last = trainer.train(examples: examples) { losses.append($0.loss) }
        print("losses: \(losses.map { String(format: "%.3f", $0) })")
        XCTAssertEqual(losses.count, 4)
        XCTAssertLessThan(last, losses[0])

        let adapterPath = TestSupport.temporaryPath("trained")
        defer { try? FileManager.default.removeItem(atPath: adapterPath) }
        try LoraAdapterWriter.write(to: adapterPath, architecture: "gpt2", alpha: config.alpha, pairs: try model.exportLoraPairs())
        let tokens = tokenizer.tokenize("\u{EE00}キシャ\u{EE01}")
        _ = try TestSupport.llamaLogits(modelPath: f16, adapterPath: adapterPath, tokens: tokens)
    }
}
