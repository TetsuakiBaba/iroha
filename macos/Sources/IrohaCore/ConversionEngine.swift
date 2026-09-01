import Foundation

/// かな漢字変換エンジンの抽象。実装を差し替えることで別モデルを利用できる。
/// 現行実装: ZenzEngine（zenz-v3 GGUF + llama.cpp）
public protocol ConversionEngine: Sendable {
    /// ひらがな読みを仮名漢字混じり文候補に変換する。
    /// - Parameters:
    ///   - reading: ひらがなの読み（例: "きょうはいいてんき"）
    ///   - context: 直前に確定した文字列（文脈条件付け用、空でも可）
    ///   - candidateCount: 返す候補数の上限（1ならグリーディ、2以上でビーム探索）
    /// - Returns: 尤度順の変換候補
    func convert(reading: String, context: String, candidateCount: Int) async throws -> [String]

    /// モデルの事前ロード（初回変換のもたつき防止）。必要のない実装は何もしなくてよい
    func prewarm() async throws
}

extension ConversionEngine {
    public func prewarm() async throws {}
}

public enum ConversionError: Error, CustomStringConvertible {
    case modelNotFound(String)
    case modelLoadFailed(String)
    case inferenceFailed(String)
    case cancelled

    public var description: String {
        switch self {
        case .modelNotFound(let path): return "モデルファイルが見つかりません: \(path)"
        case .modelLoadFailed(let reason): return "モデルの読み込みに失敗: \(reason)"
        case .inferenceFailed(let reason): return "推論に失敗: \(reason)"
        case .cancelled: return "変換がキャンセルされました"
        }
    }
}

/// ひらがな→カタカナ変換（zenzのプロンプトはカタカナ読みを要求する）
public func hiraganaToKatakana(_ hiragana: String) -> String {
    String(hiragana.unicodeScalars.map { scalar -> Character in
        // ぁ(U+3041)〜ゖ(U+3096) → ァ(U+30A1)〜ヶ(U+30F6)
        if (0x3041...0x3096).contains(scalar.value),
           let converted = Unicode.Scalar(scalar.value + 0x60) {
            return Character(converted)
        }
        return Character(scalar)
    })
}
