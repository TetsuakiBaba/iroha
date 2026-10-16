import ApplicationServices
import Cocoa
import InputMethodKit

/// デバッグ表示用: カーソルのある要素から、テキストをどれだけ取れるかを数える（2026-10-17）。
///
/// 左文脈はカーソル手前の 1 行（最大 40 文字）しか使わないが、アプリによって返せる範囲が違う
/// （全文を返すもの・カーソル付近だけのもの・何も返さないもの）。中身は出さず文字数だけを見る。
/// 経路は 2 つ:
/// - IMK: iroha が左文脈を読むのと同じ `IMKTextInput`。カーソルの前と後を実際に読んで数える
/// - AX: アクセシビリティ（要権限）。フォーカスのある要素の文字数とカーソル位置
/// どちらも同期の IPC なので、デバッグ表示が ON のときに入力の始めで 1 回だけ呼ぶ
enum FocusedTextProbe {

    /// 片側で読む上限（UTF-16 単位）。長い文書で同期の IPC が重くならないように
    static let readLimit = 100_000

    struct IMKResult: Equatable {
        /// アプリが申告する文書の長さ（UTF-16）。返さなければ nil
        var reportedLength: Int?
        /// カーソルの前・後で実際に読めた文字数。読めなければ nil
        var before: Int?
        var after: Int?
        /// 上限で読むのをやめたか
        var beforeLimited = false
        var afterLimited = false
    }

    enum AXResult: Equatable {
        case notTrusted
        case noElement
        case noText
        /// 文字数（UTF-16。アプリの申告）と、カーソル位置（返さなければ nil）
        case counted(total: Int, cursor: Int?)
    }

    static func probeIMK(_ client: IMKTextInput) -> IMKResult {
        var result = IMKResult()
        let length = client.length()
        if length > 0, length != NSNotFound { result.reportedLength = length }
        let selection = client.selectedRange()
        guard selection.location != NSNotFound else { return result }

        // 前: 文頭からカーソルまで（未確定文字列があればその手前まで）
        var end = selection.location
        let marked = client.markedRange()
        if marked.location != NSNotFound, marked.length > 0, marked.location < end { end = marked.location }
        if end == 0 {
            result.before = 0
        } else {
            let start = max(0, end - readLimit)
            result.beforeLimited = start > 0
            result.before = client.attributedSubstring(from: NSRange(location: start, length: end - start))?
                .string.count
        }

        // 後: 選択範囲の後ろから文末まで。文書の長さを申告しないアプリでは文末が分からないので読まない
        if let length = result.reportedLength {
            let afterStart = selection.location + selection.length
            if afterStart >= length {
                result.after = 0
            } else {
                let count = min(length - afterStart, readLimit)
                result.afterLimited = count < length - afterStart
                result.after = client.attributedSubstring(from: NSRange(location: afterStart, length: count))?
                    .string.count
            }
        }
        return result
    }

    static func probeAX() -> AXResult {
        guard AXIsProcessTrusted() else { return .notTrusted }
        // 応答しないアプリで入力が止まらないように、要素ごとに待ち時間を短くする
        // （システム全体の要素に設定すると、選択テキストの AI 編集など iroha の他の AX 呼び出しにも効いてしまう）
        guard let application = NSWorkspace.shared.frontmostApplication else { return .noElement }
        let appElement = AXUIElementCreateApplication(application.processIdentifier)
        AXUIElementSetMessagingTimeout(appElement, 0.2)
        guard let focused = attribute(kAXFocusedUIElementAttribute, of: appElement),
              CFGetTypeID(focused) == AXUIElementGetTypeID() else { return .noElement }
        let element = unsafeBitCast(focused, to: AXUIElement.self)
        AXUIElementSetMessagingTimeout(element, 0.2)

        let total: Int
        if let count = attribute(kAXNumberOfCharactersAttribute, of: element) as? Int {
            total = count
        } else if let value = attribute(kAXValueAttribute, of: element) as? String {
            total = value.utf16.count
        } else {
            return .noText
        }
        var cursor: Int?
        if let value = attribute(kAXSelectedTextRangeAttribute, of: element),
           CFGetTypeID(value) == AXValueGetTypeID() {
            var range = CFRange()
            if AXValueGetValue(unsafeBitCast(value, to: AXValue.self), .cfRange, &range) {
                cursor = range.location
            }
        }
        return .counted(total: total, cursor: cursor)
    }

    private static func attribute(_ name: String, of element: AXUIElement) -> CFTypeRef? {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success else { return nil }
        return value
    }
}
