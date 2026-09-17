import XCTest
import CLlama
@testable import IrohaCore

final class LoraAdapterWriterTests: XCTestCase {

    private func temporaryPath(_ name: String) -> String {
        FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-lora-test-\(ProcessInfo.processInfo.processIdentifier)-\(name).gguf").path
    }

    /// 書いたアダプタを GGUFFile で読み戻す: KV・テンソル名・ne・型・中身が一致する
    func testWriteAndReadBack() throws {
        let path = temporaryPath("roundtrip")
        defer { try? FileManager.default.removeItem(atPath: path) }

        let inFeatures = 4, outFeatures = 6, rank = 2
        let a = (0..<(rank * inFeatures)).map { Float($0) * 0.5 }          // [rank][in]
        let b = (0..<(outFeatures * rank)).map { Float($0) * -0.25 }       // [out][rank]
        let pair = try LoraAdapterWriter.Pair(baseName: "blk.0.attn_qkv.weight", inFeatures: inFeatures,
                                              outFeatures: outFeatures, rank: rank, a: a, b: b)
        try LoraAdapterWriter.write(to: path, architecture: "gpt2", alpha: 16, pairs: [pair])
        XCTAssertFalse(FileManager.default.fileExists(atPath: path + ".tmp"))

        let file = try GGUFFile(path: path)
        XCTAssertEqual(file.string("general.type"), "adapter")
        XCTAssertEqual(file.architecture, "gpt2")
        XCTAssertEqual(file.string("adapter.type"), "lora")
        XCTAssertEqual(file.float("adapter.lora.alpha"), 16)

        let tensorA = try XCTUnwrap(file.tensor(named: "blk.0.attn_qkv.weight.lora_a"))
        let tensorB = try XCTUnwrap(file.tensor(named: "blk.0.attn_qkv.weight.lora_b"))
        // llama-adapter.cpp の検証: a.ne[0] == in, b.ne[1] == out, a.ne[1] == b.ne[0] == rank
        XCTAssertEqual(tensorA.ne, [inFeatures, rank])
        XCTAssertEqual(tensorB.ne, [rank, outFeatures])
        XCTAssertEqual(tensorA.type, GGML_TYPE_F32)
        XCTAssertEqual(try file.floats(of: tensorA), a)
        XCTAssertEqual(try file.floats(of: tensorB), b)
        XCTAssertEqual(tensorA.rowMajorShape, [rank, inFeatures])
    }

    func testPairRejectsWrongElementCount() {
        XCTAssertThrowsError(try LoraAdapterWriter.Pair(baseName: "x", inFeatures: 4, outFeatures: 6, rank: 2,
                                                        a: [Float](repeating: 0, count: 7), b: [Float](repeating: 0, count: 12)))
        XCTAssertThrowsError(try LoraAdapterWriter.Pair(baseName: "x", inFeatures: 4, outFeatures: 6, rank: 2,
                                                        a: [Float](repeating: 0, count: 8), b: [Float](repeating: 0, count: 11)))
    }

    func testWriteRequiresTensors() {
        XCTAssertThrowsError(try LoraAdapterWriter.write(to: temporaryPath("empty"), architecture: "gpt2", alpha: 16, pairs: []))
    }
}
