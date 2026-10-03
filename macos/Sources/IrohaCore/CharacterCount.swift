import Foundation

/// 選択テキストの文字数。改行は数えず、空白（スペース・タブ・全角スペース等）を含めた数と
/// 含めない数の両方を持つ（Wordの「文字数（スペースを含める/含めない）」と同じ考え方）。
/// 文字は拡張書記素クラスタ単位（結合文字・絵文字は1文字）。
///
/// 英語の文を選んだときは単語数も出す（2026-10-03）。かな・漢字を1文字でも含めば日本語として文字数だけ、
/// 含まずに英字があれば英語として単語数を先に出す。割合で決めないのは、境目の数字に根拠がなく、
/// どちらの表示になるかをユーザが予想できなくなるため
public struct CharacterCount: Equatable, Sendable {
    /// 改行を除いた全文字数（空白を含む）
    public let total: Int
    /// 改行と空白を除いた文字数
    public let withoutWhitespace: Int
    /// 空白・改行で区切ったまとまりの数（Microsoft Word と同じ数え方。「well-known」「e.g.」「2026」は各1語）
    public let words: Int
    /// かな・漢字を含まず英字があるか（真なら単語数を出す）
    public let isEnglish: Bool

    public init(of text: String) {
        var total = 0
        var withoutWhitespace = 0
        var words = 0
        var inWord = false
        var hasJapanese = false
        var hasLetter = false
        for character in text {
            if character.isWhitespace || character.isNewline {
                inWord = false
            } else if !inWord {
                inWord = true
                words += 1
            }
            if !hasJapanese, Self.isJapanese(character) { hasJapanese = true }
            if !hasLetter, character.isLetter { hasLetter = true }
            if character.isNewline { continue }
            total += 1
            if !character.isWhitespace { withoutWhitespace += 1 }
        }
        self.total = total
        self.withoutWhitespace = withoutWhitespace
        self.words = words
        self.isEnglish = !hasJapanese && hasLetter
    }

    /// かな（半角カナを含む）か漢字
    private static func isJapanese(_ character: Character) -> Bool {
        character.unicodeScalars.contains { scalar in
            switch scalar.value {
            case 0x3040...0x309F, 0x30A0...0x30FF, 0x31F0...0x31FF, 0xFF66...0xFF9F: return true
            default: return scalar.properties.isIdeographic
            }
        }
    }

    /// 表示用の短い文。日本語は「12文字」（空白があれば「12文字（空白除く 10）」）、
    /// 英語は「152 words (812 chars)」
    public var summary: String {
        if isEnglish {
            return "\(words) \(words == 1 ? "word" : "words") (\(total) \(total == 1 ? "char" : "chars"))"
        }
        if withoutWhitespace == total {
            return "\(total)文字"
        }
        return "\(total)文字（空白除く \(withoutWhitespace)）"
    }
}
