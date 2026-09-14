import Foundation

/// 選択テキストの文字数。改行は数えず、空白（スペース・タブ・全角スペース等）を含めた数と
/// 含めない数の両方を持つ（Wordの「文字数（スペースを含める/含めない）」と同じ考え方）。
/// 文字は拡張書記素クラスタ単位（結合文字・絵文字は1文字）
public struct CharacterCount: Equatable, Sendable {
    /// 改行を除いた全文字数（空白を含む）
    public let total: Int
    /// 改行と空白を除いた文字数
    public let withoutWhitespace: Int

    public init(of text: String) {
        var total = 0
        var withoutWhitespace = 0
        for character in text {
            if character.isNewline { continue }
            total += 1
            if !character.isWhitespace { withoutWhitespace += 1 }
        }
        self.total = total
        self.withoutWhitespace = withoutWhitespace
    }

    /// 表示用の短い文（「12文字」。空白があれば「12文字（空白除く 10）」）
    public var summary: String {
        if withoutWhitespace == total {
            return "\(total)文字"
        }
        return "\(total)文字（空白除く \(withoutWhitespace)）"
    }
}
