import Cocoa
import InputMethodKit

/// デバッグ表示用: カーソルのある要素から、テキストをどれだけ取れるかを数える（2026-10-17）。
///
/// 左文脈はカーソル手前の 1 行（最大 40 文字）しか使わないが、アプリによって返せる範囲が違う
/// （全文を返すもの・カーソル付近だけのもの・何も返さないもの）。中身は出さず文字数だけを見る。
/// iroha が左文脈を読むのと同じ `IMKTextInput` で、カーソルの前と後を実際に読んで数える。
/// 同期の IPC で 1 文字目の表示を待たせるので、デバッグ表示が ON のときに入力の始めで 1 回だけ呼ぶ。
/// アクセシビリティ（AX）では数えない（2026-10-17 に取り下げ）。VS Code などの Chromium 系のアプリは、
/// AX の問い合わせが来て初めて画面の部品の木を作るので、最初の 1 回が 30〜45ms かかり、その後もアプリが
/// 木を保ち続ける。AX を使うのは選択テキストの AI 編集だけにする
enum FocusedTextProbe {

    /// テキストを読むのに使ってよい時間。最初の 1 回（片側 `firstChunk`）は必ず読み、
    /// そこまでで超えていればそれ以上は読まない（2026-10-17。それまでは片側 10 万文字まで読んでいた）
    static let timeBudget: Duration = .milliseconds(30)
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
}
