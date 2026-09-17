import Foundation
import CLlama

/// llama.cpp が読める LoRA アダプタ（GGUF）を書き出す。
///
/// 形式は `llama-adapter.cpp` の検証に合わせる:
/// - KV: `general.type="adapter"`, `general.architecture`（ベースと同じ）, `adapter.type="lora"`, `adapter.lora.alpha`
/// - テンソル: `<ベースのテンソル名>.lora_a`（ne=[in, rank]）と `.lora_b`（ne=[rank, out]）。F32
/// - 適用は `W·x + (alpha/rank)·Bᵀ(Aᵀx)`。`mul_mat(a, x)` は a の各行（長さ in）と x の内積なので
///   `lora_a` のメモリ配置は行優先で [rank][in]、`lora_b` は [out][rank]
public enum LoraAdapterWriter {

    /// 1 つの重みに対する LoRA の A/B（行優先の Float 配列）
    public struct Pair: Sendable {
        /// ベースモデルのテンソル名（例 "blk.0.attn_qkv.weight"）
        public let baseName: String
        public let inFeatures: Int
        public let outFeatures: Int
        public let rank: Int
        /// [rank][in] 行優先（`x @ A` の A:[in, rank] を転置したもの）
        public let a: [Float]
        /// [out][rank] 行優先（`h @ B` の B:[rank, out] を転置したもの）
        public let b: [Float]

        public init(baseName: String, inFeatures: Int, outFeatures: Int, rank: Int, a: [Float], b: [Float]) throws {
            guard a.count == rank * inFeatures else {
                throw GGUFError.invalidShape("\(baseName).lora_a: 要素数 \(a.count) ≠ rank \(rank) × in \(inFeatures)")
            }
            guard b.count == outFeatures * rank else {
                throw GGUFError.invalidShape("\(baseName).lora_b: 要素数 \(b.count) ≠ out \(outFeatures) × rank \(rank)")
            }
            self.baseName = baseName
            self.inFeatures = inFeatures
            self.outFeatures = outFeatures
            self.rank = rank
            self.a = a
            self.b = b
        }
    }

    /// アダプタを書き出す。`.tmp` に書いてから rename する（部分ファイルを残さない）
    public static func write(to path: String, architecture: String, alpha: Float, pairs: [Pair]) throws {
        guard !pairs.isEmpty else { throw GGUFError.writeFailed("テンソルがありません") }
        guard let gguf = gguf_init_empty() else { throw GGUFError.writeFailed("gguf_init_empty") }
        defer { gguf_free(gguf) }

        gguf_set_val_str(gguf, "general.type", "adapter")
        gguf_set_val_str(gguf, "general.architecture", architecture)
        gguf_set_val_str(gguf, "adapter.type", "lora")
        gguf_set_val_f32(gguf, "adapter.lora.alpha", alpha)

        // テンソルのメタデータ用コンテキスト（データは自前のバッファを指させる）
        var params = ggml_init_params()
        params.mem_size = pairs.count * 2 * ggml_tensor_overhead() + 1024
        params.mem_buffer = nil
        params.no_alloc = true
        guard let ggml = ggml_init(params) else { throw GGUFError.writeFailed("ggml_init") }
        defer { ggml_free(ggml) }

        var buffers: [UnsafeMutablePointer<Float>] = []
        defer { buffers.forEach { $0.deallocate() } }

        func addTensor(name: String, ne0: Int, ne1: Int, values: [Float]) throws {
            guard let tensor = ggml_new_tensor_2d(ggml, GGML_TYPE_F32, Int64(ne0), Int64(ne1)) else {
                throw GGUFError.writeFailed("ggml_new_tensor_2d \(name)")
            }
            let buffer = UnsafeMutablePointer<Float>.allocate(capacity: values.count)
            buffers.append(buffer)
            values.withUnsafeBufferPointer { buffer.update(from: $0.baseAddress!, count: values.count) }
            tensor.pointee.data = UnsafeMutableRawPointer(buffer)
            ggml_set_name(tensor, name)
            gguf_add_tensor(gguf, tensor)
        }

        for pair in pairs {
            try addTensor(name: pair.baseName + ".lora_a", ne0: pair.inFeatures, ne1: pair.rank, values: pair.a)
            try addTensor(name: pair.baseName + ".lora_b", ne0: pair.rank, ne1: pair.outFeatures, values: pair.b)
        }

        let temporary = path + ".tmp"
        try? FileManager.default.removeItem(atPath: temporary)
        try FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent,
                                                withIntermediateDirectories: true)
        guard gguf_write_to_file(gguf, temporary, false) else {
            try? FileManager.default.removeItem(atPath: temporary)
            throw GGUFError.writeFailed(temporary)
        }
        try? FileManager.default.removeItem(atPath: path)
        try FileManager.default.moveItem(atPath: temporary, toPath: path)
    }
}
