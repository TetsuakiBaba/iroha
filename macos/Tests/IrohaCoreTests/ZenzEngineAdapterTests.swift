import XCTest
import CLlama
@testable import IrohaCore

/// LoRA アダプタの適用を実モデルで確認する（`IROHA_TEST_MODEL` か repo の training/ にある zenz を使う。
/// 無ければスキップ）
final class ZenzEngineAdapterTests: XCTestCase {

    private static let modelPath: String? = {
        if let path = ProcessInfo.processInfo.environment["IROHA_TEST_MODEL"], !path.isEmpty { return path }
        let candidates = ["../training/zenz-v3.1-small-Q5_K_M.gguf", ZenzEngine.defaultModelPath]
        return candidates.first { FileManager.default.fileExists(atPath: $0) }
    }()

    private func requireModel() throws -> String {
        guard let path = Self.modelPath else {
            throw XCTSkip("モデルが無いためスキップ（IROHA_TEST_MODEL でGGUFを指定）")
        }
        return path
    }

    private func temporaryPath(_ name: String) -> String {
        FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-adapter-test-\(ProcessInfo.processInfo.processIdentifier)-\(name).gguf").path
    }

    /// ベースの形に合わせた LoRA（a はランダム、b は与えた値）を書く
    private func writeAdapter(base: GGUFFile, to path: String, bValue: Float, rank: Int = 4) throws {
        var pairs: [LoraAdapterWriter.Pair] = []
        for name in ["blk.0.attn_qkv.weight", "blk.0.attn_output.weight"] {
            let tensor = try XCTUnwrap(base.tensor(named: name))
            let inFeatures = tensor.ne[0], outFeatures = tensor.ne[1]
            let a = (0..<(rank * inFeatures)).map { _ in Float.random(in: -0.05...0.05) }
            let b = [Float](repeating: bValue, count: outFeatures * rank)
            pairs.append(try LoraAdapterWriter.Pair(baseName: name, inFeatures: inFeatures, outFeatures: outFeatures,
                                                    rank: rank, a: a, b: b))
        }
        try LoraAdapterWriter.write(to: path, architecture: try XCTUnwrap(base.architecture), alpha: 8, pairs: pairs)
    }

    /// B = 0 のアダプタは変換結果を変えない（形式・名前・適用の経路がすべて正しいことの確認）
    func testZeroAdapterKeepsConversion() async throws {
        let modelPath = try requireModel()
        let base = try GGUFFile(path: modelPath)
        let adapterPath = temporaryPath("zero")
        defer { try? FileManager.default.removeItem(atPath: adapterPath) }
        try writeAdapter(base: base, to: adapterPath, bValue: 0)

        let plain = ZenzEngine(modelPath: modelPath)
        let adapted = ZenzEngine(modelPath: modelPath, adapterPath: adapterPath)
        for reading in ["きしゃがきしゃできしゃした", "きょうはいいてんきですね"] {
            let expected = try await plain.convert(reading: reading, context: "", candidateCount: 1)
            let actual = try await adapted.convert(reading: reading, context: "", candidateCount: 1)
            XCTAssertEqual(actual, expected, reading)
        }
    }

    /// B が大きいアダプタは読み込めて、出力が変わる（適用が実際に効いている）
    func testLargeAdapterChangesConversion() async throws {
        let modelPath = try requireModel()
        let base = try GGUFFile(path: modelPath)
        let adapterPath = temporaryPath("large")
        defer { try? FileManager.default.removeItem(atPath: adapterPath) }
        try writeAdapter(base: base, to: adapterPath, bValue: 2.0)

        let plain = ZenzEngine(modelPath: modelPath)
        let adapted = ZenzEngine(modelPath: modelPath, adapterPath: adapterPath)
        let reading = "きしゃがきしゃできしゃした"
        let expected = try await plain.convert(reading: reading, context: "", candidateCount: 1)
        let actual = try await adapted.convert(reading: reading, context: "", candidateCount: 1)
        XCTAssertNotEqual(actual, expected)
    }

    /// 存在しないアダプタ・別アーキのアダプタは黙ってベースで動かず例外になる
    func testBrokenAdapterThrows() async throws {
        let modelPath = try requireModel()
        let missing = ZenzEngine(modelPath: modelPath, adapterPath: temporaryPath("missing"))
        do {
            _ = try await missing.convert(reading: "あ", context: "", candidateCount: 1)
            XCTFail("例外になるはず")
        } catch let error as ConversionError {
            guard case .modelNotFound = error else { return XCTFail("\(error)") }
        }

        let wrongArch = temporaryPath("wrongarch")
        defer { try? FileManager.default.removeItem(atPath: wrongArch) }
        let pair = try LoraAdapterWriter.Pair(baseName: "blk.0.attn_qkv.weight", inFeatures: 4, outFeatures: 6, rank: 2,
                                              a: [Float](repeating: 0, count: 8), b: [Float](repeating: 0, count: 12))
        try LoraAdapterWriter.write(to: wrongArch, architecture: "llama", alpha: 8, pairs: [pair])
        let mismatched = ZenzEngine(modelPath: modelPath, adapterPath: wrongArch)
        do {
            _ = try await mismatched.convert(reading: "あ", context: "", candidateCount: 1)
            XCTFail("例外になるはず")
        } catch let error as ConversionError {
            guard case .modelLoadFailed = error else { return XCTFail("\(error)") }
        }
    }

    /// 語彙だけのトークナイザ: 出力タグが 1 トークンになり、ZenzEngine と同じ規則で分割する
    func testVocabTokenizer() throws {
        let modelPath = try requireModel()
        let tokenizer = try VocabTokenizer(modelPath: modelPath)
        XCTAssertFalse(tokenizer.architecture.isEmpty)
        let tag = tokenizer.outputTagTokens
        XCTAssertFalse(tag.isEmpty)
        // zenz は eos_token_id(2)=<s> が誤りで、終端は </s>(3)
        XCTAssertEqual(tokenizer.piece(tokenizer.terminator), "</s>")
        let line = "\u{EE02}本日は\u{EE00}キシャ\u{EE01}記者"
        let tokens = tokenizer.tokenize(line)
        // 学習行をそのまま encode できて、損失位置の次が「記」
        let examples = try TrainingDataBuilder.encode(lines: [line], tokenize: { tokenizer.tokenize($0) },
                                                      eos: tokenizer.terminator, outputTag: tag)
        XCTAssertEqual(examples.count, 1)
        XCTAssertEqual(tokenizer.piece(examples[0].tokens[examples[0].lossFrom + 1]), "記")
        XCTAssertGreaterThan(tokens.count, 5)
    }
}
