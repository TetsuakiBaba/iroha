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
/// どちらも同期の IPC なので、デバッグ表示が ON のときに入力の始めで 1 回だけ呼ぶ。
/// IMK はメインスレッドで呼ぶので 1 文字目の表示を待たせる。AX は裏のスレッドから呼べる
enum FocusedTextProbe {

    /// IMK でテキストを読むのに使ってよい時間。最初の 1 回（片側 `firstChunk`）は必ず読み、
    /// そこまでで超えていればそれ以上は読まない（2026-10-17。片側 10 万文字まで読んでいたら
    /// 1 文字目の表示が目に見えて遅れた）
    static let timeBudget: Duration = .microseconds(30)
    /// 最初に読む量と、時間が残っているときに次に読む量の倍率（UTF-16 単位）
    static let firstChunk = 1_024
    static let chunkGrowth = 8

    struct IMKResult: Equatable {
        /// アプリが申告する文書の長さ（UTF-16）。返さなければ nil
        var reportedLength: Int?
        /// カーソルの前・後で実際に読めた文字数。読めなければ nil
        var before: Int?
        var after: Int?
        /// 時間切れで読むのをやめたか（読めた数より多くある）
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
        let clock = ContinuousClock()
        let deadline = clock.now + timeBudget
        var result = IMKResult()
        let length = client.length()
        if length > 0, length != NSNotFound { result.reportedLength = length }
        let selection = client.selectedRange()
        guard selection.location != NSNotFound else { return result }

        // 前: 文頭からカーソルまで（未確定文字列があればその手前まで）
        var end = selection.location
        let marked = client.markedRange()
        if marked.location != NSNotFound, marked.length > 0, marked.location < end { end = marked.location }
        // 後: 選択範囲の後ろから文末まで。文書の長さを申告しないアプリでは文末が分からないので読まない
        let afterStart = selection.location + selection.length
        let afterEnd = result.reportedLength

        // 両側とも最初の 1 回は読む。そのあとは時間が残っている間だけ、量を増やしながら続きを読む
        var before = Side(lower: 0, upper: end, readsBackward: true)
        var after = afterEnd.map { Side(lower: afterStart, upper: max(afterStart, $0), readsBackward: false) }
        before.readNext(client)
        after?.readNext(client)
        while clock.now < deadline, before.canContinue || after?.canContinue == true {
            if before.canContinue {
                before.readNext(client)
            } else {
                after?.readNext(client)
            }
        }
        result.before = before.count
        result.beforeLimited = before.canContinue
        result.after = after?.count
        result.afterLimited = after?.canContinue ?? false
        return result
    }

    /// カーソルの片側。`readsBackward` ならカーソル（`upper`）から文頭へ、でなければカーソル（`lower`）から文末へ読む
    private struct Side {
        var lower: Int
        var upper: Int
        let readsBackward: Bool
        /// 読めた文字数。アプリがテキストを返さなければ nil
        var count: Int? = 0
        private var chunk = FocusedTextProbe.firstChunk

        init(lower: Int, upper: Int, readsBackward: Bool) {
            self.lower = lower
            self.upper = upper
            self.readsBackward = readsBackward
        }

        var canContinue: Bool { count != nil && lower < upper }

        mutating func readNext(_ client: IMKTextInput) {
            guard canContinue else { return }
            let length = min(chunk, upper - lower)
            let range = readsBackward
                ? NSRange(location: upper - length, length: length)
                : NSRange(location: lower, length: length)
            guard let text = client.attributedSubstring(from: range)?.string else {
                count = nil
                return
            }
            count = (count ?? 0) + text.count
            if readsBackward { upper -= length } else { lower += length }
            chunk *= FocusedTextProbe.chunkGrowth
        }
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

        // 文字数だけを聞く。テキストそのもの（kAXValue）は長い文書だと重いので取らない
        guard let total = attribute(kAXNumberOfCharactersAttribute, of: element) as? Int else { return .noText }
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
