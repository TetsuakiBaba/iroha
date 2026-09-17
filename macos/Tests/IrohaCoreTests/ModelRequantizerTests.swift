import XCTest
import CLlama
@testable import IrohaCore

final class ModelRequantizerTests: XCTestCase {

    private static let modelPath: String? = {
        if let path = ProcessInfo.processInfo.environment["IROHA_TEST_MODEL"], !path.isEmpty { return path }
        let candidates = ["../training/zenz-v3.1-small-Q5_K_M.gguf", ZenzEngine.defaultModelPath]
        return candidates.first { FileManager.default.fileExists(atPath: $0) }
    }()

    /// Q5_K_M → f16: 2次元の weight は F16、norm/bias（1次元）と位置埋め込みは F32 のまま
    func testRequantizeToF16() throws {
        guard let modelPath = Self.modelPath else { throw XCTSkip("モデルが無いためスキップ") }
        let output = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-requant-test-\(ProcessInfo.processInfo.processIdentifier).gguf")
        defer { try? FileManager.default.removeItem(at: output) }

        let start = Date()
        let result = try ModelRequantizer.ensureF16(basePath: modelPath, output: output)
        print("f16 化: \(String(format: "%.1f", Date().timeIntervalSince(start)))秒")
        XCTAssertEqual(result, output)

        let base = try GGUFFile(path: modelPath)
        let f16 = try GGUFFile(path: output.path)
        XCTAssertEqual(f16.architecture, base.architecture)
        XCTAssertEqual(f16.tensors.count, base.tensors.count)
        for tensor in f16.tensors {
            XCTAssertEqual(tensor.ne, base.tensor(named: tensor.name)?.ne, tensor.name)
            XCTAssertTrue(tensor.type == GGML_TYPE_F16 || tensor.type == GGML_TYPE_F32,
                          "\(tensor.name) type=\(tensor.type.rawValue)")
        }
        XCTAssertEqual(f16.tensor(named: "blk.0.attn_qkv.weight")?.type, GGML_TYPE_F16)
        // 既に f16 なら再変換せずそのまま返す
        XCTAssertEqual(try ModelRequantizer.ensureF16(basePath: output.path), output)
    }
}
