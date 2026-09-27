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

    /// アプリからの読み取りの結果。読めなかったときは理由を区別する
    /// （デバッグ表示で、左文脈が効かない原因がアプリ側かどうかを切り分けるため）
    enum ReadResult: Equatable {
        /// 設定で OFF
        case disabled
        /// アプリがカーソル位置を返さない
        case noCursor
        /// カーソル位置が 0（文頭）。前に文字があるのにこれなら、アプリが位置を正しく返していない
        case atStart
        /// カーソル位置は返したが、テキストを返さない
        case noText
        /// 読めた（左文脈の形にしたもの。改行だけなら空）
        case text(String)

        /// 左文脈。読めなかったら nil（呼び出し側は確定文字列の蓄積で代える）
        var context: String? {
            switch self {
            case .atStart: return ""
            case .text(let text): return text
            case .disabled, .noCursor, .noText: return nil
            }
        }
    }

    /// カーソル（選択範囲の先頭）手前のテキストを左文脈の形で返す。
    /// カーソル位置やテキストを返さないアプリ（Electron系・ターミナル等）では nil。
    /// カーソルが先頭にあれば空文字列（文脈なし）を返す。
    ///
    /// 同期IPCなので合成の開始時と確定直後にだけ呼ぶこと（キー入力ごとには呼ばない）
    static func read(from client: IMKTextInput) -> String? {
        inspect(from: client).context
    }

    /// `read` と同じ読み取りを、読めなかった理由つきで返す
    static func inspect(from client: IMKTextInput) -> ReadResult {
        guard isEnabled else { return .disabled }
        let selection = client.selectedRange()
        guard selection.location != NSNotFound else { return .noCursor }
        guard selection.location > 0 else { return .atStart }
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
        else { return .noText }
        return .text(LeftContext.normalize(text))
    }
}
