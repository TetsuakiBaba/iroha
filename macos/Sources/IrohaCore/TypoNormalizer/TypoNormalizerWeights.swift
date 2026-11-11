import Foundation
#if canImport(Accelerate)
import Accelerate
#endif

/// Typo Normalizer の重みと設定（`export_model.py` が書き出す manifest.json / weights.bin）。
///
/// 書き出し側の仕様は `training/typo-normalizer/SWIFT-PORT.md` §2 が正。
/// weights.bin は全テンソルを **little-endian で連結しただけ**のもので、どこに何があるかは
/// manifest.json の `tensors`（name / shape / offset / count）だけが知っている。
struct TypoNormalizerManifest: Decodable {

    struct Config: Decodable {
        let dModel: Int
        let nHead: Int
        let encLayers: Int
        let decLayers: Int
        let dFF: Int
        let maxLength: Int

        enum CodingKeys: String, CodingKey {
            case dModel = "d_model"
            case nHead = "nhead"
            case encLayers = "enc_layers"
            case decLayers = "dec_layers"
            case dFF = "d_ff"
            case maxLength = "max_len"
        }
    }

    struct TensorInfo: Decodable {
        let name: String
        let shape: [Int]
        /// 展開した後（float32）の位置と要素数
        let offset: Int
        let count: Int
        /// dtype が "mixed" のときだけ: このテンソルの形式（"float16" か "q6"）と weights.bin の中のバイト位置・バイト数
        let format: String?
        let byteOffset: Int?
        let bytes: Int?

        enum CodingKeys: String, CodingKey {
            case name, shape, offset, count, format, bytes
            case byteOffset = "byte_offset"
        }
    }

    let config: Config
    let vocabSize: Int
    /// 出力ヘッドの幅。入力語彙を左文脈用に広げた run では `vocabSize` より小さい
    let outputSize: Int
    /// id → 文字。先頭4つは特殊トークン（`<pad> <s> </s> <unk>`）
    let vocab: [String]
    /// "float32-le"（書き出しの既定）・"float16-le"（`iroha-cli typo shrink` で縮めたもの）・
    /// "mixed"（`iroha-cli typo shrink --q6`。テンソルごとに q6 か float16。2026-11-11、iroha-typo-v3 から）
    let dtype: String
    let totalFloats: Int
    let tensors: [TensorInfo]

    enum CodingKeys: String, CodingKey {
        case config
        case vocabSize = "vocab_size"
        case outputSize = "n_out"
        case vocab, dtype
        case totalFloats = "total_floats"
        case tensors
    }
}

/// weights.bin をメモリに展開して、テンソル名から先頭ポインタを引けるようにしたもの。
///
/// float16 で書かれていれば読み込み時に float32 へ戻す（計算は常に float32。
/// float16・q6 で書かれていても読み込み時に float32 へ戻す（計算は常に float32。iroha-typo-v3 は 46.2M パラメータで
/// 展開すると 185MB。配布の大きさを減らすのが目的で、推論中に変換するコストは持ち込まない）。
final class TypoNormalizerWeights {

    enum LoadError: Error, CustomStringConvertible {
        case unsupportedDType(String)
        case sizeMismatch(expected: Int, actual: Int)
        case missingTensor(String)
        case shapeMismatch(name: String, expected: [Int], actual: [Int])
        case outOfRange(name: String, offset: Int, count: Int)

        var description: String {
            switch self {
            case .unsupportedDType(let dtype):
                return "対応していない dtype です: \(dtype)"
            case .sizeMismatch(let expected, let actual):
                return "weights.bin の大きさが manifest と合いません（期待 \(expected) 要素、実際 \(actual) 要素）"
            case .missingTensor(let name):
                return "manifest にテンソル \(name) がありません"
            case .shapeMismatch(let name, let expected, let actual):
                return "テンソル \(name) の形が違います（期待 \(expected)、実際 \(actual)）"
            case .outOfRange(let name, let offset, let count):
                return "テンソル \(name) が weights.bin の外を指しています（offset \(offset)、count \(count)）"
            }
        }
    }

    private let storage: UnsafeMutableBufferPointer<Float>
    private let index: [String: TypoNormalizerManifest.TensorInfo]

    init(manifest: TypoNormalizerManifest, data: Data) throws {
        let floats: [Float]
        switch manifest.dtype {
        case "float32-le":
            guard data.count == manifest.totalFloats * 4 else {
                throw LoadError.sizeMismatch(expected: manifest.totalFloats * 4, actual: data.count)
            }
            floats = Self.decodeFloat32LE(data, count: manifest.totalFloats)
        case "float16-le":
            guard data.count == manifest.totalFloats * 2 else {
                throw LoadError.sizeMismatch(expected: manifest.totalFloats * 2, actual: data.count)
            }
            floats = Self.decodeFloat16LE(data, count: manifest.totalFloats)
        case "mixed":
            floats = try Self.decodeMixed(data, manifest: manifest)
        default:
            throw LoadError.unsupportedDType(manifest.dtype)
        }
        storage = UnsafeMutableBufferPointer<Float>.allocate(capacity: floats.count)
        _ = storage.initialize(fromContentsOf: floats)
        index = Dictionary(uniqueKeysWithValues: manifest.tensors.map { ($0.name, $0) })
    }

    deinit {
        storage.deallocate()
    }

    /// テンソルの先頭ポインタ。形が manifest と違えば読み込み時点で弾く
    /// （移植のずれは「動くが微妙に違う」形で出るのが一番たちが悪いので、名前だけでなく形も見る）
    func tensor(_ name: String, shape expected: [Int]) throws -> UnsafePointer<Float> {
        guard let info = index[name] else { throw LoadError.missingTensor(name) }
        guard info.shape == expected else {
            throw LoadError.shapeMismatch(name: name, expected: expected, actual: info.shape)
        }
        // manifest が壊れていても範囲外を読まない（IROHA_TYPO_MODEL で任意の場所を指せるため）
        guard info.offset >= 0, info.count >= 0, info.offset + info.count <= storage.count else {
            throw LoadError.outOfRange(name: name, offset: info.offset, count: info.count)
        }
        return UnsafePointer(storage.baseAddress!.advanced(by: info.offset))
    }

    // MARK: - バイト列 → Float

    private static func decodeFloat32LE(_ data: Data, count: Int) -> [Float] {
        var result = [Float](repeating: 0, count: count)
        result.withUnsafeMutableBytes { destination in
            data.copyBytes(to: destination.bindMemory(to: UInt8.self), count: count * 4)
        }
        // x86_64 / arm64 はどちらもリトルエンディアンなのでバイト順の入れ替えは要らない。
        // 将来ビッグエンディアンへ移すことがあれば、ここで UInt32 として読んで littleEndian を通すこと
        assert(1.littleEndian == 1, "ビッグエンディアン環境では weights.bin のバイト順変換が要る")
        return result
    }

    /// テンソルごとに形式の違う weights.bin を float32 に展開する（`TypoWeightConversion` の q6 と float16）
    private static func decodeMixed(_ data: Data, manifest: TypoNormalizerManifest) throws -> [Float] {
        var result = [Float](repeating: 0, count: manifest.totalFloats)
        for info in manifest.tensors {
            guard let format = info.format, let byteOffset = info.byteOffset, let bytes = info.bytes,
                  byteOffset >= 0, bytes >= 0, byteOffset + bytes <= data.count,
                  info.offset >= 0, info.count >= 0, info.offset + info.count <= result.count
            else { throw LoadError.outOfRange(name: info.name, offset: info.offset, count: info.count) }
            let chunk = data.subdata(in: byteOffset..<(byteOffset + bytes))
            let values: [Float]
            switch format {
            case "float16":
                guard bytes == info.count * 2 else {
                    throw LoadError.sizeMismatch(expected: info.count * 2, actual: bytes)
                }
                values = decodeFloat16LE(chunk, count: info.count)
            case "q6":
                guard bytes == TypoWeightConversion.q6Bytes(count: info.count) else {
                    throw LoadError.sizeMismatch(expected: TypoWeightConversion.q6Bytes(count: info.count), actual: bytes)
                }
                values = TypoWeightConversion.dequantizeQ6([UInt8](chunk), count: info.count)
            default:
                throw LoadError.unsupportedDType(format)
            }
            result.replaceSubrange(info.offset..<(info.offset + info.count), with: values)
        }
        return result
    }

    private static func decodeFloat16LE(_ data: Data, count: Int) -> [Float] {
        var halves = [UInt16](repeating: 0, count: count)
        halves.withUnsafeMutableBytes { destination in
            data.copyBytes(to: destination.bindMemory(to: UInt8.self), count: count * 2)
        }
        var result = [Float](repeating: 0, count: count)
        for i in 0..<count { result[i] = Self.halfToFloat(UInt16(littleEndian: halves[i])) }
        return result
    }

    /// IEEE 754 binary16 → binary32。`Float16` は環境によって使えないので自前で展開する
    static func halfToFloat(_ half: UInt16) -> Float {
        let sign = UInt32(half & 0x8000) << 16
        let exponent = Int((half >> 10) & 0x1F)
        let mantissa = UInt32(half & 0x03FF)
        if exponent == 0 {
            if mantissa == 0 { return Float(bitPattern: sign) }             // ±0
            // 非正規化数（|x| < 2^-14）: 先頭の 1 が隠れビットの位置に来るまで左へ寄せ、
            // 寄せたぶんだけ指数を下げる。half の非正規化数は 2^-24 刻みなので、
            // mantissa=1 が 2^-24、mantissa=0x200 が 2^-15 になること
            var m = mantissa
            var e = 0
            repeat {
                m <<= 1
                e -= 1
            } while m & 0x0400 == 0
            m &= 0x03FF
            let bits = sign | UInt32(127 - 15 + e + 1) << 23 | (m << 13)
            return Float(bitPattern: bits)
        }
        if exponent == 0x1F {                                                // Inf / NaN
            return Float(bitPattern: sign | 0x7F80_0000 | (mantissa << 13))
        }
        return Float(bitPattern: sign | UInt32(exponent - 15 + 127) << 23 | (mantissa << 13))
    }

    /// binary32 → binary16（最近接偶数丸め）。重みを float16 に落とす変換器が使う
    static func floatToHalf(_ value: Float) -> UInt16 {
        let bits = value.bitPattern
        let sign = UInt16((bits >> 16) & 0x8000)
        let exponent = Int((bits >> 23) & 0xFF) - 127 + 15
        let mantissa = bits & 0x007F_FFFF
        if (bits >> 23) & 0xFF == 0xFF {                                     // Inf / NaN
            return sign | 0x7C00 | (mantissa == 0 ? 0 : 0x0200)
        }
        if exponent >= 0x1F { return sign | 0x7C00 }                         // オーバーフロー → Inf
        if exponent <= 0 {
            if exponent < -10 { return sign }                                // アンダーフロー → ±0
            let m = (mantissa | 0x0080_0000) >> UInt32(1 - exponent)
            // 丸め（最近接偶数）
            let rounded = (m + 0x0FFF + ((m >> 13) & 1)) >> 13
            return sign | UInt16(truncatingIfNeeded: rounded)
        }
        let rounded = UInt32(exponent) << 23 | mantissa
        let half = (rounded + 0x0FFF + ((rounded >> 13) & 1)) >> 13
        return sign | UInt16(truncatingIfNeeded: half)
    }
}

/// 重みを float16 に落とす変換器（`iroha-cli typo shrink`）が使う入り口。
/// 推論そのものは常に float32 で行うので、ここは書き出し専用
public enum TypoWeightConversion {
    public static func floatToHalf(_ value: Float) -> UInt16 { TypoNormalizerWeights.floatToHalf(value) }
    public static func halfToFloat(_ half: UInt16) -> Float { TypoNormalizerWeights.halfToFloat(half) }

    // MARK: - q6（2026-11-11）
    //
    // 32 個ずつの区切りに float16 の倍率 d を 1 つ持ち、各値を -32〜31 の整数 q にする（x ≈ q × d）。
    // d は区切りの中で絶対値が最大の値（符号つき）を -32 で割ったもの（llama.cpp の q4_0 / q5_0 と同じ決め方）。
    // 1 区切り = 倍率 2 バイト + 6 ビット × 32 = 24 バイト（q + 32 を 4 つずつ 3 バイトに詰める）= 26 バイト。
    // training/typo-normalizer/quant_sim.py の "q6" と同じ計算（丸めは最近接偶数）で、精度はそちらで測ってある
    // （xxlarge 20 epoch で float32 と test の完全一致に差なし。MLX-HANDOFF.md 15 節）

    public static let q6Block = 32

    /// 量子化するテンソルか（線形層の重み。埋め込み・位置埋め込み・出力層・1 次元のものは float16 のまま）
    public static func isQ6Target(name: String, shape: [Int]) -> Bool {
        shape.count == 2 && shape[1] % q6Block == 0
            && !["emb.", "pos_src.", "pos_tgt.", "head."].contains { name.hasPrefix($0) }
    }

    public static func q6Bytes(count: Int) -> Int { count / q6Block * 26 }

    public static func quantizeQ6(_ values: ArraySlice<Float>) -> [UInt8] {
        precondition(values.count % q6Block == 0)
        var out: [UInt8] = []
        out.reserveCapacity(q6Bytes(count: values.count))
        var index = values.startIndex
        while index < values.endIndex {
            let block = values[index..<(index + q6Block)]
            var peak: Float = 0
            for x in block where abs(x) > abs(peak) { peak = x }
            let scaleHalf = floatToHalf(peak / -32)
            let scale = halfToFloat(scaleHalf)
            let divisor = scale == 0 ? 1 : scale
            out.append(UInt8(scaleHalf & 0xFF))
            out.append(UInt8(scaleHalf >> 8))
            var packed: [UInt32] = []
            for x in block {
                let q = min(31, max(-32, (x / divisor).rounded(.toNearestOrEven)))
                packed.append(UInt32(Int(q) + 32))
            }
            for i in stride(from: 0, to: q6Block, by: 4) {
                let word = packed[i] | packed[i + 1] << 6 | packed[i + 2] << 12 | packed[i + 3] << 18
                out.append(UInt8(word & 0xFF))
                out.append(UInt8((word >> 8) & 0xFF))
                out.append(UInt8((word >> 16) & 0xFF))
            }
            index += q6Block
        }
        return out
    }

    public static func dequantizeQ6(_ bytes: [UInt8], count: Int) -> [Float] {
        var result = [Float](repeating: 0, count: count)
        var position = 0
        for block in 0..<(count / q6Block) {
            let scale = halfToFloat(UInt16(bytes[position]) | UInt16(bytes[position + 1]) << 8)
            position += 2
            for group in 0..<(q6Block / 4) {
                let word = UInt32(bytes[position]) | UInt32(bytes[position + 1]) << 8 | UInt32(bytes[position + 2]) << 16
                position += 3
                for k in 0..<4 {
                    let q = Int((word >> (6 * UInt32(k))) & 0x3F) - 32
                    result[block * q6Block + group * 4 + k] = Float(q) * scale
                }
            }
        }
        return result
    }
}
