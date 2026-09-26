import Foundation
import XCTest
import CLlama
import MLX
@testable import IrohaTrain
@testable import IrohaCore

/// テスト共通: モデルの場所と MLX の metallib
enum TestSupport {
    /// パッケージルート（macos/）。`swift test` の cwd に依存しないよう #filePath から求める
    static let packageRoot = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent()

    static let baseModelPath: String? = {
        if let path = ProcessInfo.processInfo.environment["IROHA_TEST_MODEL"], !path.isEmpty { return path }
        let candidates = [packageRoot.deletingLastPathComponent().appendingPathComponent("training/zenz-v3.1-small-Q5_K_M.gguf").path,
                          ZenzEngine.defaultModelPath]
        return candidates.first { FileManager.default.fileExists(atPath: $0) }
    }()

    /// エンコーダ・デコーダ型（T5）のテスト用モデル。`IROHA_TEST_T5_MODEL` か training/t5/ の f16 GGUF
    /// （training/ はリポジトリに入っていないので、無ければスキップ）
    static func requireT5Model() throws -> String {
        if let path = ProcessInfo.processInfo.environment["IROHA_TEST_T5_MODEL"], !path.isEmpty { return path }
        let path = packageRoot.deletingLastPathComponent().appendingPathComponent("training/t5/iroha-t5-e12d2-full-100m-f16.gguf").path
        guard FileManager.default.fileExists(atPath: path) else {
            throw XCTSkip("T5 モデルが無いためスキップ（IROHA_TEST_T5_MODEL で GGUF を指定）")
        }
        return try ModelRequantizer.ensureF16(basePath: path).path
    }

    /// MLX が探せる場所に metallib を用意する。`swift test` では Bundle.main が xctest なので、
    /// 最後の手段（cwd の default.metallib）に合わせて cwd をリソースバンドルに移す
    static func prepareMLX() throws {
        let candidates = ["debug", "release"].map {
            packageRoot.appendingPathComponent(".build/\($0)/mlx-swift_Cmlx.bundle", isDirectory: true)
        }
        guard let bundle = candidates.first(where: {
            FileManager.default.fileExists(atPath: $0.appendingPathComponent("default.metallib").path)
        }) else {
            throw XCTSkip("mlx.metallib が無いためスキップ（scripts/build-mlx-metallib.sh debug を実行）")
        }
        FileManager.default.changeCurrentDirectoryPath(bundle.path)
    }

    static func requireModel() throws -> String {
        guard let path = baseModelPath else { throw XCTSkip("モデルが無いためスキップ（IROHA_TEST_MODEL でGGUFを指定）") }
        return path
    }

    /// f16 版（キャッシュ）
    static func f16Model() throws -> String {
        try ModelRequantizer.ensureF16(basePath: try requireModel()).path
    }

    static func temporaryPath(_ name: String, ext: String = "gguf") -> String {
        FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-train-test-\(ProcessInfo.processInfo.processIdentifier)-\(name).\(ext)").path
    }

    /// llama.cpp で最後の位置のロジットを取る（MLX 実装との突き合わせ用）
    static func llamaLogits(modelPath: String, adapterPath: String? = nil, tokens: [Int32]) throws -> [Float] {
        llama_backend_init()
        var modelParams = llama_model_default_params()
        modelParams.n_gpu_layers = 99
        guard let model = llama_model_load_from_file(modelPath, modelParams) else { throw ConversionError.modelLoadFailed(modelPath) }
        defer { llama_model_free(model) }
        var contextParams = llama_context_default_params()
        contextParams.n_ctx = 256
        contextParams.n_batch = 256
        guard let context = llama_init_from_model(model, contextParams) else { throw ConversionError.modelLoadFailed("ctx") }
        defer { llama_free(context) }
        var adapter: OpaquePointer?
        if let adapterPath {
            guard let loaded = llama_adapter_lora_init(model, adapterPath) else { throw ConversionError.modelLoadFailed(adapterPath) }
            adapter = loaded
            var adapters: [OpaquePointer?] = [loaded]
            var scales: [Float] = [1]
            guard llama_set_adapters_lora(context, &adapters, 1, &scales) == 0 else { throw ConversionError.modelLoadFailed("set adapter") }
        }
        defer { if let adapter { llama_adapter_lora_free(adapter) } }
        var input = tokens
        let status = input.withUnsafeMutableBufferPointer { llama_decode(context, llama_batch_get_one($0.baseAddress, Int32($0.count))) }
        guard status == 0 else { throw ConversionError.inferenceFailed("decode=\(status)") }
        let vocab = Int(llama_vocab_n_tokens(llama_model_get_vocab(model)))
        guard let logits = llama_get_logits_ith(context, -1) else { throw ConversionError.inferenceFailed("logits") }
        return Array(UnsafeBufferPointer(start: logits, count: vocab))
    }

    /// T5: `source` をエンコードし、`decoderTokens` をデコーダに流した各位置のロジット
    static func llamaT5Logits(modelPath: String, adapterPath: String? = nil, source: [Int32],
                              decoderTokens: [Int32]) throws -> [[Float]] {
        llama_backend_init()
        var modelParams = llama_model_default_params()
        modelParams.n_gpu_layers = 99
        guard let model = llama_model_load_from_file(modelPath, modelParams) else { throw ConversionError.modelLoadFailed(modelPath) }
        defer { llama_model_free(model) }
        var contextParams = llama_context_default_params()
        contextParams.n_ctx = 256
        contextParams.n_batch = 256
        contextParams.n_ubatch = 256
        guard let context = llama_init_from_model(model, contextParams) else { throw ConversionError.modelLoadFailed("ctx") }
        defer { llama_free(context) }
        var adapter: OpaquePointer?
        if let adapterPath {
            guard let loaded = llama_adapter_lora_init(model, adapterPath) else { throw ConversionError.modelLoadFailed(adapterPath) }
            adapter = loaded
            var adapters: [OpaquePointer?] = [loaded]
            var scales: [Float] = [1]
            guard llama_set_adapters_lora(context, &adapters, 1, &scales) == 0 else { throw ConversionError.modelLoadFailed("set adapter") }
        }
        defer { if let adapter { llama_adapter_lora_free(adapter) } }
        var input = source
        let encoded = input.withUnsafeMutableBufferPointer { llama_encode(context, llama_batch_get_one($0.baseAddress, Int32($0.count))) }
        guard encoded == 0 else { throw ConversionError.inferenceFailed("encode=\(encoded)") }
        var batch = llama_batch_init(Int32(decoderTokens.count), 0, 1)
        defer { llama_batch_free(batch) }
        for (index, token) in decoderTokens.enumerated() {
            batch.token[index] = token
            batch.pos[index] = Int32(index)
            batch.n_seq_id[index] = 1
            batch.seq_id[index]![0] = 0
            batch.logits[index] = 1
        }
        batch.n_tokens = Int32(decoderTokens.count)
        let status = llama_decode(context, batch)
        guard status == 0 else { throw ConversionError.inferenceFailed("decode=\(status)") }
        let vocab = Int(llama_vocab_n_tokens(llama_model_get_vocab(model)))
        return try decoderTokens.indices.map { index in
            guard let logits = llama_get_logits_ith(context, Int32(index)) else { throw ConversionError.inferenceFailed("logits") }
            return Array(UnsafeBufferPointer(start: logits, count: vocab))
        }
    }

    static func maxAbsDifference(_ a: [Float], _ b: [Float]) -> Float {
        zip(a, b).map { abs($0 - $1) }.max() ?? .infinity
    }

    static func argmax(_ a: [Float]) -> Int {
        a.indices.max { a[$0] < a[$1] } ?? 0
    }
}
