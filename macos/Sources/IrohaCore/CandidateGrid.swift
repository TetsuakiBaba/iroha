/// 候補ウィンドウの並びと選択の動き（表示は持たない）。
///
/// 候補は 9 件ずつの列に分ける（列の中の番号 1〜9 で選べる）。最初は今の候補がある 1 列だけを見せ、
/// Tab で列を横に並べた表に広げる。表に広げている間だけ ←→ で隣の列へ移る
/// （閉じているときの ←→ は文節の移動なので、ここでは扱わない）。
///
/// 候補の並びは列優先（1 列目の 9 件の次が 2 列目の先頭）なので、↓・Space は表に広げていても
/// 1 列のときと同じく「次の候補」へ進む
public struct CandidateGrid: Equatable, Sendable {
    /// 1 列に並べる件数（番号キー 1〜9 に合わせる）
    public static let rowsPerColumn = 9

    public let count: Int
    public private(set) var selected: Int
    /// 列を横に並べた表に広げているか
    public private(set) var isExpanded = false

    public init(count: Int, selected: Int = 0) {
        self.count = max(0, count)
        self.selected = count > 0 ? min(max(0, selected), count - 1) : 0
    }

    public var columnCount: Int { (count + Self.rowsPerColumn - 1) / Self.rowsPerColumn }
    public var selectedColumn: Int { selected / Self.rowsPerColumn }
    public var selectedRow: Int { selected % Self.rowsPerColumn }

    /// 列 `column` に入る候補の番号の範囲
    public func indices(ofColumn column: Int) -> Range<Int> {
        let start = column * Self.rowsPerColumn
        return start..<min(start + Self.rowsPerColumn, count)
    }

    /// 1 列に収まらないときだけ広げる意味がある
    public var canExpand: Bool { columnCount > 1 }

    /// 次の候補へ（最後の次は先頭に戻る）
    public mutating func next() {
        guard count > 0 else { return }
        selected = (selected + 1) % count
    }

    /// 前の候補へ（先頭の前は最後に戻る）
    public mutating func previous() {
        guard count > 0 else { return }
        selected = (selected - 1 + count) % count
    }

    /// 表に広げているときに隣の列の同じ行へ移る。移る先の列が短ければその列の最後の候補。
    /// 端の列からさらに外へは移らない。動いたら真
    @discardableResult
    public mutating func moveColumn(by delta: Int) -> Bool {
        let column = selectedColumn + delta
        guard isExpanded, delta != 0, (0..<columnCount).contains(column) else { return false }
        let range = indices(ofColumn: column)
        selected = min(range.lowerBound + selectedRow, range.upperBound - 1)
        return true
    }

    /// 番号キー（1〜9）が指す候補。今の候補がある列の中で数える。その番号の候補が無ければ nil
    public func index(forNumber number: Int) -> Int? {
        guard (1...Self.rowsPerColumn).contains(number) else { return nil }
        let index = selectedColumn * Self.rowsPerColumn + number - 1
        return index < count ? index : nil
    }

    /// 表に広げる・1 列に戻す。1 列に収まる候補では何もしない。変わったら真
    @discardableResult
    public mutating func toggleExpanded() -> Bool {
        guard canExpand || isExpanded else { return false }
        isExpanded.toggle()
        return true
    }
}
