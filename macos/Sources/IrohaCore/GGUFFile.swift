import Foundation
import CLlama

public enum GGUFError: Error, CustomStringConvertible {
    case cannotOpen(String)
    case tensorNotFound(String)
    case readFailed(String)
    case invalidShape(String)
    case writeFailed(String)

    public var description: String {
        switch self {
        case .cannotOpen(let path): return "GGUFファイルを開けません: \(path)"
        case .tensorNotFound(let name): return "テンソルがありません: \(name)"
        case .readFailed(let reason): return "GGUFの読み込みに失敗: \(reason)"
        case .invalidShape(let reason): return "テンソルの形が不正: \(reason)"
        case .writeFailed(let reason): return "GGUFの書き出しに失敗: \(reason)"
        }
    }
}

/// GGUFファイルのメタデータとテンソルの配置を読む（読み取り専用）。
///
/// llama.cpp の `gguf_init_from_file(no_alloc)` でヘッダだけを読み、テンソルの中身は
/// `data(of:)` で必要なものだけファイルから取り出す。追加学習（`iroha-train`）が
/// ベースモデルの重みを MLX に流し込むときと、アダプタの形状検証・テストに使う。
/// トークナイザの語彙のような配列値は読まない（`VocabTokenizer` が llama.cpp 経由で扱う）
public final class GGUFFile: @unchecked Sendable {

    public struct Tensor: Sendable, Equatable {
        public let name: String
        /// ggml の次元順（`ne[0]` がメモリ上で連続する最内の次元。行列は [in, out] に見える）
        public let ne: [Int]
        public let type: ggml_type
        /// データ領域の先頭からのバイトオフセット
        public let offset: Int
        public let byteSize: Int

        /// 行優先で読んだときの形（`ne` の逆順。行列は [out, in]）
        public var rowMajorShape: [Int] { ne.reversed() }
    }

    public enum Value: Sendable, Equatable {
        case string(String)
        case uint32(UInt32)
        case int32(Int32)
        case float(Float)
        case bool(Bool)
        case uint64(UInt64)
    }

    public let path: String
    public let tensors: [Tensor]
    public let values: [String: Value]
    private let dataOffset: Int

    public init(path: String) throws {
        self.path = path
        let params = gguf_init_params(no_alloc: true, ctx: nil)
        guard let ctx = gguf_init_from_file(path, params) else { throw GGUFError.cannotOpen(path) }
        defer { gguf_free(ctx) }

        dataOffset = Int(gguf_get_data_offset(ctx))

        var values: [String: Value] = [:]
        for index in 0..<gguf_get_n_kv(ctx) {
            let key = String(cString: gguf_get_key(ctx, index))
            switch gguf_get_kv_type(ctx, index) {
            case GGUF_TYPE_STRING: values[key] = .string(String(cString: gguf_get_val_str(ctx, index)))
            case GGUF_TYPE_UINT32: values[key] = .uint32(gguf_get_val_u32(ctx, index))
            case GGUF_TYPE_INT32: values[key] = .int32(gguf_get_val_i32(ctx, index))
            case GGUF_TYPE_FLOAT32: values[key] = .float(gguf_get_val_f32(ctx, index))
            case GGUF_TYPE_BOOL: values[key] = .bool(gguf_get_val_bool(ctx, index))
            case GGUF_TYPE_UINT64: values[key] = .uint64(gguf_get_val_u64(ctx, index))
            default: continue  // 配列など
            }
        }
        self.values = values

        var tensors: [Tensor] = []
        for index in 0..<gguf_get_n_tensors(ctx) {
            let name = String(cString: gguf_get_tensor_name(ctx, index))
            let nePointer = gguf_get_tensor_ne(ctx, index)!
            // GGML_MAX_DIMS 個あるが末尾の 1 は次元ではない
            var ne = (0..<Int(GGML_MAX_DIMS)).map { Int(nePointer[$0]) }
            while ne.count > 1, ne.last == 1 { ne.removeLast() }
            tensors.append(Tensor(name: name, ne: ne, type: gguf_get_tensor_type(ctx, index),
                                  offset: Int(gguf_get_tensor_offset(ctx, index)),
                                  byteSize: Int(gguf_get_tensor_size(ctx, index))))
        }
        self.tensors = tensors
    }

    public func string(_ key: String) -> String? {
        if case .string(let value) = values[key] { return value }
        return nil
    }

    public func uint32(_ key: String) -> UInt32? {
        switch values[key] {
        case .uint32(let value): return value
        case .int32(let value) where value >= 0: return UInt32(value)
        default: return nil
        }
    }

    public func float(_ key: String) -> Float? {
        if case .float(let value) = values[key] { return value }
        return nil
    }

    /// `general.architecture`（"gpt2" / "llama" / "t5" など）
    public var architecture: String? { string("general.architecture") }

    public func tensor(named name: String) -> Tensor? {
        tensors.first { $0.name == name }
    }

    /// テンソルの中身をファイルから読む（型はそのまま。f16 なら 2 バイト/要素）
    public func data(of tensor: Tensor) throws -> Data {
        guard let handle = FileHandle(forReadingAtPath: path) else { throw GGUFError.cannotOpen(path) }
        defer { try? handle.close() }
        do {
            try handle.seek(toOffset: UInt64(dataOffset + tensor.offset))
            guard let data = try handle.read(upToCount: tensor.byteSize), data.count == tensor.byteSize else {
                throw GGUFError.readFailed("\(tensor.name) を \(tensor.byteSize) バイト読めませんでした")
            }
            return data
        } catch let error as GGUFError {
            throw error
        } catch {
            throw GGUFError.readFailed("\(tensor.name): \(error)")
        }
    }

    /// テンソルを Float32 の配列として読む（F32 / F16 のみ。量子化型は不可）
    public func floats(of tensor: Tensor) throws -> [Float] {
        let data = try data(of: tensor)
        switch tensor.type {
        case GGML_TYPE_F32:
            return data.withUnsafeBytes { Array($0.bindMemory(to: Float.self)) }
        case GGML_TYPE_F16:
            return data.withUnsafeBytes { raw in
                raw.bindMemory(to: UInt16.self).map { Float(Float16(bitPattern: $0)) }
            }
        default:
            throw GGUFError.readFailed("\(tensor.name) は F32/F16 ではありません（type=\(tensor.type.rawValue)）")
        }
    }
}
