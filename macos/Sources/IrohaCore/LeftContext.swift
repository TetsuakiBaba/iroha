import Foundation

/// LLMに左文脈として渡す文字列の整形。
///
/// 文脈は「コントローラが確定した文字列の蓄積」または「アプリのカーソル手前のテキスト」から
/// 作られる。どちらも同じ形（カーソルのある行だけ・末尾40文字）にそろえる。
public enum LeftContext {
    /// 左文脈として保持する最大文字数（zenz-v3の学習設定に合わせる。ZenzEngineも同じ長さで切る）
    public static let maxLength = 40

    /// アプリから読んだテキストを左文脈の形にする。
    /// 最後の改行より後ろ（カーソルのある行）だけを残し、行頭の空白を除いて末尾 `maxLength` 文字にする。
    /// azooKey（`SegmentsManager.getCleanLeftSideContext`）と同じ扱い。前の行まで文脈に入れると、
    /// 箇条書きや別の話題の行に引っ張られる（2026-09-30: 前の行の「神と化しています」に引かれて
    /// 「風呂上がりで」の続きの「かみとかしています」が「神と化しています」になった）
    public static func normalize(_ text: String) -> String {
        let line = text.split(omittingEmptySubsequences: false, whereSeparator: \.isNewline).last ?? Substring(text)
        return String(line.drop(while: \.isWhitespace).suffix(maxLength))
    }
}
