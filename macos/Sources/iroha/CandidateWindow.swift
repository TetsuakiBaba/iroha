import Cocoa
import IrohaCore

/// 文節の候補ウィンドウ（スペースを2回押したときに出る一覧）。
///
/// 以前は macOS の IMKCandidates を使っていたが、番号キーの扱いや表の広げ方を iroha 側で決められない
/// ため自前で描く（2026-10-03）。並びと選択の動きは `CandidateGrid`（IrohaCore）が持ち、この窓は描くだけ。
///
/// - 最初は今の候補がある 1 列（9 件）だけを縦に出す。番号 1〜9 はその列の中で数える
/// - 候補が 1 列に収まらなければ Tab で列を横に並べた表に広げる。画面の幅に収まらない列は、
///   選んでいる列が見えるように左右をずらして出す
/// - キーボードフォーカスは取らない（nonactivating）。候補のクリックは受ける
///
/// IMKのコールバックと同じくメインスレッドから使う
final class CandidateWindow {
    static let shared = CandidateWindow()

    private let panel: NSPanel
    private let gridView = CandidateGridView()
    /// 置く基準（今の文節の先頭の、行の高さを持つ幅0の矩形。スクリーン座標）
    private var anchor = NSRect.zero
    /// 表に広げたとき、左端に出している列
    private var firstVisibleColumn = 0
    /// カーソル行とウィンドウの間隔
    private let caretGap: CGFloat = 2
    /// 画面の左右に残す余白
    private let screenMargin: CGFloat = 16

    private init() {
        panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 120, height: 28),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: true
        )
        // 予測の小窓（CaretPanel）と同じくフルスクリーンのアプリの上にも出す
        panel.level = .popUpMenu
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.hidesOnDeactivate = false
        panel.isReleasedWhenClosed = false
        panel.animationBehavior = .none

        let background = NSVisualEffectView()
        background.material = .popover
        background.blendingMode = .behindWindow
        background.state = .active
        background.wantsLayer = true
        background.layer?.cornerRadius = 6
        background.layer?.masksToBounds = true
        panel.contentView = background
        background.addSubview(gridView)
    }

    var isVisible: Bool { panel.isVisible }

    /// 候補を出す。`anchor` が nil（カーソル位置を教えてくれないアプリ）ならマウスポインタの位置に出す。
    /// 候補をクリックすると `onClick` にその番号が来る
    func show(_ candidates: [String], grid: CandidateGrid, near anchor: NSRect?,
              onClick: @escaping (Int) -> Void) {
        let mouse = NSEvent.mouseLocation
        self.anchor = anchor ?? NSRect(x: mouse.x, y: mouse.y, width: 0, height: 0)
        firstVisibleColumn = 0
        gridView.texts = candidates
        gridView.onClick = onClick
        update(grid)
    }

    /// 選択・広げ方が変わったときに描き直す。位置の基準は `show` のときのまま
    func update(_ grid: CandidateGrid) {
        gridView.grid = grid
        gridView.visibleColumns = grid.isExpanded
            ? visibleColumns(for: grid) : grid.selectedColumn..<(grid.selectedColumn + 1)
        let size = gridView.fittingSize
        gridView.frame = NSRect(origin: .zero, size: size)
        gridView.needsDisplay = true
        place(size: size)
    }

    func hide() {
        guard panel.isVisible else { return }
        panel.orderOut(nil)
    }

    /// 表に広げたときに出す列。画面の幅に収まるだけ出し、選んでいる列が必ず入るよう左端をずらす
    private func visibleColumns(for grid: CandidateGrid) -> Range<Int> {
        let maxWidth = (screen?.visibleFrame.width ?? 1200) - screenMargin * 2 - gridView.horizontalInset * 2
        let widths = (0..<grid.columnCount).map { gridView.columnWidth($0) }
        let selected = grid.selectedColumn
        var first = min(firstVisibleColumn, selected)
        // 選んでいる列が右にはみ出すなら、左端を進める
        while first < selected, widths[first...selected].reduce(0, +) > maxWidth {
            first += 1
        }
        var end = selected + 1
        var total = widths[first..<end].reduce(0, +)
        while end < widths.count, total + widths[end] <= maxWidth {
            total += widths[end]
            end += 1
        }
        // 右端まで出し切って余りがあれば、左にも広げる
        while first > 0, total + widths[first - 1] <= maxWidth {
            first -= 1
            total += widths[first]
        }
        firstVisibleColumn = first
        return first..<end
    }

    private var screen: NSScreen? {
        NSScreen.screens.first { $0.frame.contains(anchor.origin) } ?? NSScreen.main
    }

    /// 行の直下に置く。画面の下に収まらなければ行の上に出す
    private func place(size: NSSize) {
        var origin = NSPoint(x: anchor.minX, y: anchor.minY - caretGap - size.height)
        if let visible = screen?.visibleFrame {
            if origin.y < visible.minY {
                origin.y = anchor.maxY + caretGap
            }
            origin.x = min(max(origin.x, visible.minX), max(visible.minX, visible.maxX - size.width))
        }
        panel.setFrame(NSRect(origin: origin, size: size), display: true)
        panel.orderFrontRegardless()
    }
}

/// 候補を 9 件ずつの列に分けて描く。各行の左に列の中の番号（1〜9）を出す。
/// クリックは非アクティブなアプリの窓でも最初の1回で届くようにする（`acceptsFirstMouse`）
private final class CandidateGridView: NSView {
    var texts: [String] = [] {
        didSet { textWidths = texts.map { ($0 as NSString).size(withAttributes: [.font: font]).width } }
    }
    var grid = CandidateGrid(count: 0)
    var visibleColumns: Range<Int> = 0..<0
    var onClick: ((Int) -> Void)?

    private var textWidths: [CGFloat] = []

    private let font = NSFont.systemFont(ofSize: 15)
    private let numberFont = NSFont.monospacedDigitSystemFont(ofSize: 11, weight: .regular)
    private let footerFont = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize)
    let horizontalInset: CGFloat = 4
    private let verticalPadding: CGFloat = 4
    private let cellPadding: CGFloat = 8
    private let numberGap: CGFloat = 6
    private let rowSpacing: CGFloat = 1
    private let footerGap: CGFloat = 12

    private var rowHeight: CGFloat { ceil(font.ascender - font.descender + font.leading) + 6 }
    private var numberWidth: CGFloat { ("9" as NSString).size(withAttributes: [.font: numberFont]).width }
    private var footerHeight: CGFloat { grid.canExpand ? ceil(footerFont.ascender - footerFont.descender) + 6 : 0 }

    override var isFlipped: Bool { true }

    /// 列の幅。1 列のときは全候補の最長に合わせる（Space で列をまたいでも窓の幅が変わらないように）
    func columnWidth(_ column: Int) -> CGFloat {
        let range = grid.isExpanded ? grid.indices(ofColumn: column) : 0..<texts.count
        let textWidth = range.map { textWidths[$0] }.max() ?? 0
        return ceil(cellPadding * 2 + numberWidth + numberGap + textWidth)
    }

    /// 1 列に収まらないなら 9 行ぶんの高さに固定する（最後の短い列でも窓の高さが変わらないように）
    private var rows: Int { min(grid.count, CandidateGrid.rowsPerColumn) }

    private var footerText: (left: String, right: String) {
        let more = grid.isExpanded
            ? (visibleColumns.lowerBound > 0 ? "‹ " : "") + "\(grid.selected + 1) / \(grid.count)"
                + (visibleColumns.upperBound < grid.columnCount ? " ›" : "")
            : "\(grid.selected + 1) / \(grid.count)"
        return (more, grid.isExpanded ? "Tab で 1 列に戻す" : "Tab で一覧を広げる")
    }

    override var fittingSize: NSSize {
        var width = visibleColumns.map { columnWidth($0) }.reduce(0, +)
        if grid.canExpand {
            let footer = footerText
            let footerWidth = (footer.left as NSString).size(withAttributes: [.font: footerFont]).width
                + footerGap + (footer.right as NSString).size(withAttributes: [.font: footerFont]).width
                + cellPadding * 2
            width = max(width, ceil(footerWidth))
        }
        let rowsHeight = CGFloat(rows) * rowHeight + CGFloat(max(0, rows - 1)) * rowSpacing
        return NSSize(width: ceil(width + horizontalInset * 2),
                      height: ceil(verticalPadding * 2 + rowsHeight + footerHeight))
    }

    /// 候補 `index` の行の矩形（出していない列なら nil）
    private func cellRect(_ index: Int) -> NSRect? {
        let column = index / CandidateGrid.rowsPerColumn
        guard visibleColumns.contains(column) else { return nil }
        let x = horizontalInset + (visibleColumns.lowerBound..<column).map { columnWidth($0) }.reduce(0, +)
        let row = index % CandidateGrid.rowsPerColumn
        return NSRect(x: x, y: verticalPadding + CGFloat(row) * (rowHeight + rowSpacing),
                      width: columnWidth(column), height: rowHeight)
    }

    override func draw(_ dirtyRect: NSRect) {
        for (position, column) in visibleColumns.enumerated() {
            // 表に広げたときは列の間に区切り線を引く
            if position > 0, let first = cellRect(grid.indices(ofColumn: column).lowerBound) {
                NSColor.separatorColor.setFill()
                NSRect(x: first.minX, y: verticalPadding, width: 1,
                       height: CGFloat(rows) * (rowHeight + rowSpacing) - rowSpacing).fill()
            }
            // 番号キーが効くのは選んでいる候補がある列だけなので、ほかの列の番号は薄くする
            let numbersActive = column == grid.selectedColumn
            for index in grid.indices(ofColumn: column) {
                guard let rect = cellRect(index) else { continue }
                drawCell(index, in: rect, numberActive: numbersActive)
            }
        }
        if grid.canExpand { drawFooter() }
    }

    private func drawCell(_ index: Int, in rect: NSRect, numberActive: Bool) {
        let isSelected = index == grid.selected
        if isSelected {
            NSColor.selectedContentBackgroundColor.setFill()
            NSBezierPath(roundedRect: rect.insetBy(dx: 2, dy: 0), xRadius: 4, yRadius: 4).fill()
        }
        let numberColor: NSColor = isSelected
            ? .alternateSelectedControlTextColor
            : (numberActive ? .secondaryLabelColor : .quaternaryLabelColor)
        let number = "\(index % CandidateGrid.rowsPerColumn + 1)" as NSString
        let numberAttributes: [NSAttributedString.Key: Any] = [.font: numberFont, .foregroundColor: numberColor]
        let numberSize = number.size(withAttributes: numberAttributes)
        number.draw(at: NSPoint(x: rect.minX + cellPadding, y: rect.minY + (rect.height - numberSize.height) / 2),
                    withAttributes: numberAttributes)

        let textAttributes: [NSAttributedString.Key: Any] = [
            .font: font,
            .foregroundColor: isSelected ? NSColor.alternateSelectedControlTextColor : NSColor.labelColor,
        ]
        let text = texts[index] as NSString
        let textSize = text.size(withAttributes: textAttributes)
        text.draw(at: NSPoint(x: rect.minX + cellPadding + numberWidth + numberGap,
                              y: rect.minY + (rect.height - textSize.height) / 2),
                  withAttributes: textAttributes)
    }

    private func drawFooter() {
        let footer = footerText
        let attributes: [NSAttributedString.Key: Any] = [.font: footerFont, .foregroundColor: NSColor.tertiaryLabelColor]
        let y = bounds.height - verticalPadding - footerHeight + 3
        (footer.left as NSString).draw(at: NSPoint(x: horizontalInset + cellPadding, y: y), withAttributes: attributes)
        let rightWidth = (footer.right as NSString).size(withAttributes: attributes).width
        (footer.right as NSString).draw(
            at: NSPoint(x: bounds.width - horizontalInset - cellPadding - rightWidth, y: y), withAttributes: attributes)
    }

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        let shown = visibleColumns.flatMap { grid.indices(ofColumn: $0) }
        guard let index = shown.first(where: { cellRect($0)?.contains(point) == true }) else { return }
        onClick?(index)
    }
}
