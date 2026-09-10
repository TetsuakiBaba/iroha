import Cocoa
import InputMethodKit
import IrohaCore

/// 変換の左文脈をアプリの実テキスト（カーソル手前）から取る設定と、その読み取り。
///
/// 既定ON。OFFのとき、またはアプリがテキストを返さないときは、iroha自身が確定した文字列の
/// 蓄積（`recentCommitted`）を文脈にする。アプリから取ると、既存の文章の途中にカーソルを
/// 置いて入力するときや、フォーカスを移した直後でも正しい文脈で変換できる
enum DocumentContextSettings {

    static let enabledKey = "documentContext"

    static var isEnabled: Bool {
        UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? true
    }

    /// カーソル（選択範囲の先頭）手前のテキストを左文脈の形で返す。
    /// カーソル位置やテキストを返さないアプリ（Electron系・ターミナル等）では nil。
    /// カーソルが先頭にあれば空文字列（文脈なし）を返す。
    ///
    /// 同期IPCなので合成の開始時と確定直後にだけ呼ぶこと（キー入力ごとには呼ばない）
    static func read(from client: IMKTextInput) -> String? {
        guard isEnabled else { return nil }
        let selection = client.selectedRange()
        guard selection.location != NSNotFound else { return nil }
        guard selection.location > 0 else { return "" }
        // 未確定文字列があればその手前から読む（合成開始時には無いはずだが念のため）
        let marked = client.markedRange()
        var end = selection.location
        if marked.location != NSNotFound, marked.length > 0, marked.location < end {
            end = marked.location
        }
        // サロゲートペアを含んでも40文字そろうよう、UTF-16単位で倍読む
        let start = max(0, end - LeftContext.maxLength * 2)
        guard end > start,
              let text = client.attributedSubstring(from: NSRange(location: start, length: end - start))?.string
        else { return nil }
        return LeftContext.normalize(text)
    }
}
