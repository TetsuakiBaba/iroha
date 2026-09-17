import Foundation
import CLlama

/// 量子化済み GGUF（Q5_K_M など）を f16 に戻した GGUF を作る（`llama_model_quantize` の再量子化）。
///
/// 追加学習（MLX）は量子化型を読めないので、ベースモデルの f16 版を用意する。量子化で失われた
/// 精度は戻らないが、それは推論が実際に見ている重みそのものなので、LoRA の学習対象としてはむしろ都合がよい。
/// 出力は数百MBになるため、Dropbox 等で同期されるデータフォルダではなく `~/Library/Caches/iroha/` に置く
public enum ModelRequantizer {

    /// f16 キャッシュの既定フォルダ
    public static var cacheDirectory: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first!
            .appendingPathComponent("iroha/models-f16", isDirectory: true)
    }

    /// `basePath` に対応する f16 キャッシュのパス（サイズと更新日時を名前に入れて、ベースが変わったら別ファイルになる）
    public static func cachedF16Path(for basePath: String) -> URL {
        let attributes = try? FileManager.default.attributesOfItem(atPath: basePath)
        let size = (attributes?[.size] as? NSNumber)?.int64Value ?? 0
        let modified = Int((attributes?[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0)
        let name = URL(fileURLWithPath: basePath).deletingPathExtension().lastPathComponent
        return cacheDirectory.appendingPathComponent("\(name)-\(size)-\(modified)-f16.gguf")
    }

    /// f16 版を返す（キャッシュがあればそれ、無ければ作る）。既に f16/f32 のモデルならそのまま返す
    @discardableResult
    public static func ensureF16(basePath: String, output: URL? = nil) throws -> URL {
        let base = try GGUFFile(path: basePath)
        let quantized = base.tensors.contains { $0.type != GGML_TYPE_F32 && $0.type != GGML_TYPE_F16 }
        guard quantized else { return URL(fileURLWithPath: basePath) }

        let target = output ?? cachedF16Path(for: basePath)
        if FileManager.default.fileExists(atPath: target.path) { return target }
        try FileManager.default.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)

        let temporary = target.appendingPathExtension("tmp")
        try? FileManager.default.removeItem(at: temporary)
        llama_backend_init()
        var params = llama_model_quantize_default_params()
        params.ftype = LLAMA_FTYPE_MOSTLY_F16
        params.allow_requantize = true  // 入力が Q5_K/Q6_K なので必須（既定では量子化済みテンソルを弾く）
        params.quantize_output_tensor = true
        let status = llama_model_quantize(basePath, temporary.path, &params)
        guard status == 0 else {
            try? FileManager.default.removeItem(at: temporary)
            throw GGUFError.writeFailed("llama_model_quantize=\(status): \(basePath)")
        }
        try FileManager.default.moveItem(at: temporary, to: target)
        return target
    }
}
