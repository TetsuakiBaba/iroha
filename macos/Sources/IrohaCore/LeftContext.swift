import Foundation

/// LLMに左文脈として渡す文字列の整形。
///
/// 文脈は「コントローラが確定した文字列の蓄積」または「アプリのカーソル手前のテキスト」から
/// 作られる。どちらも同じ形（改行なし・末尾40文字）にそろえる。
public enum LeftContext {
    /// 左文脈として保持する最大文字数（zenz-v3の学習設定に合わせる。ZenzEngineも同じ長さで切る）
    public static let maxLength = 40

    /// アプリから読んだテキストを左文脈の形にする。
    /// 改行は取り除いて段落をつなげる（確定文字列の蓄積では改行は入らないので同じ形になる。
    /// 前の段落も話題の手がかりになるので捨てない）。末尾 `maxLength` 文字だけ残す
    public static func normalize(_ text: String) -> String {
        String(text.filter { !$0.isNewline }.suffix(maxLength))
    }
}
