import Foundation
#if canImport(Accelerate)
import Accelerate
#endif

/// Typo Normalizer の重みと設定（`export_model.py` が書き出す manifest.json / weights.bin）。
///
/// 書き出し側の仕様は `experiments/typo-normalizer/SWIFT-PORT.md` §2 が正。
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
        let offset: Int
        let count: Int
    }

    let config: Config
    let vocabSize: Int
    /// 出力ヘッドの幅。入力語彙を左文脈用に広げた run では `vocabSize` より小さい
    let outputSize: Int
    /// id → 文字。先頭4つは特殊トークン（`<pad> <s> </s> <unk>`）
    let vocab: [String]
    /// "float32-le"（書き出しの既定）または "float16-le"（`convert-typo-weights.swift` で縮めたもの）
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
/// 3.2M パラメータなので展開しても 12.8MB で、推論中の変換コストを持ち込む意味がない）。
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
}
