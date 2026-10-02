import Cocoa

/// カーソルの近くに出す小さなフローティングウィンドウ。
/// 予測変換の候補（入力履歴から最大3件を縦に並べる）と、打ち間違いを直したときの知らせ、
/// 句読点スタイル（Control+.）を切り替えたときの知らせ（どちらも一行）に使う。
///
/// 未確定文字列（マークテキスト）には触らない。予測をマークテキストに混ぜると、検索欄の
/// インクリメンタル検索やエディタの補完が予測文まで拾ってしまい、薄い表示になるかもアプリ任せになる。
/// azooKeyの予測候補と同じく別ウィンドウにすることで、Tabを押すまでアプリのテキストは一切変わらない。
/// 打ち間違いの知らせも同じ事情で別ウィンドウにする（マークテキストの属性はアプリが無視することがあり、
/// ライブ変換中は読みのどこが直ったかを変換結果の上では示せない）。
///
/// パネルは1枚しか持たない。予測と知らせが重なって出る事故が構造的に起きないようにするため。
/// キーボードフォーカスは取らない（nonactivating）。マウスは予測の候補を出している間だけ受け
/// （クリックした候補を入れる）、知らせのときは透過する。
/// IMKのコールバックと同じくメインスレッドから使う
final class CaretPanel {
    static let shared = CaretPanel()

    private let panel: NSPanel
    private let label: NSTextField
    private let hint: NSTextField
    private let candidateList = CandidateListView()
    private let padding = NSEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
    private let hintGap: CGFloat = 8
    /// カーソル行とウィンドウの間隔
    private let caretGap: CGFloat = 2

    private init() {
        panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 120, height: 28),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: true
        )
        // 候補ウィンドウと同じくフルスクリーンのアプリの上にも出す
        panel.level = .popUpMenu
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.ignoresMouseEvents = true
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

        label = NSTextField(labelWithString: "")
        label.font = .systemFont(ofSize: NSFont.systemFontSize)
        label.lineBreakMode = .byClipping
        background.addSubview(label)

        hint = NSTextField(labelWithString: "")
        hint.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        hint.textColor = .tertiaryLabelColor
        background.addSubview(hint)

        candidateList.isHidden = true
        background.addSubview(candidateList)
    }

    /// 予測の候補を縦に並べてカーソル行の直下に出す。`selected` は Tab で選んでいる候補（nil なら未選択）。
    /// 候補をクリックすると `onClick` にその番号が来る
    func showCandidates(_ texts: [String], selected: Int?, near caretRect: NSRect,
                        onClick: @escaping (Int) -> Void) {
        label.isHidden = true
        hint.isHidden = true
        candidateList.isHidden = false
        candidateList.update(texts: texts, selected: selected, onClick: onClick)
        let size = candidateList.fittingSize
        candidateList.frame = NSRect(origin: .zero, size: size)
        panel.ignoresMouseEvents = false
        place(size: size, near: caretRect)
    }

    /// 書式付きの一行をカーソル行の直下に出す。画面の下に収まらなければ行の上に出す。
    /// `hint` が nil ならヒント欄を畳む
    func show(_ text: NSAttributedString, hint hintText: String?, near caretRect: NSRect) {
        label.isHidden = false
        hint.isHidden = false
        candidateList.isHidden = true
        panel.ignoresMouseEvents = true
        label.attributedStringValue = text
        label.sizeToFit()
        hint.stringValue = hintText ?? ""
        hint.sizeToFit()
        let gap = hintText == nil ? 0 : hintGap
        let hintWidth = hintText == nil ? 0 : hint.frame.width

        let width = padding.left + label.frame.width + gap + hintWidth + padding.right
        let height = max(label.frame.height, hint.frame.height) + padding.top + padding.bottom
        label.frame.origin = NSPoint(x: padding.left, y: (height - label.frame.height) / 2)
        hint.frame.origin = NSPoint(
            x: padding.left + label.frame.width + gap, y: (height - hint.frame.height) / 2)
        place(size: NSSize(width: width, height: height), near: caretRect)
    }

    private func place(size: NSSize, near caretRect: NSRect) {
        let width = size.width
        let height = size.height
        let screen = NSScreen.screens.first { $0.frame.contains(caretRect.origin) } ?? NSScreen.main
        var origin = NSPoint(x: caretRect.minX, y: caretRect.minY - caretGap - height)
        if let visible = screen?.visibleFrame {
            if origin.y < visible.minY {
                origin.y = caretRect.maxY + caretGap
            }
            origin.x = min(max(origin.x, visible.minX), max(visible.minX, visible.maxX - width))
        }
        panel.setFrame(NSRect(origin: origin, size: NSSize(width: width, height: height)), display: true)
        panel.orderFrontRegardless()
        // デバッグ表示も行の下に出るので、この窓を避けて置き直させる
        DeveloperOverlay.shared.relayoutIfVisible()
    }

    /// 出ているときの位置（デバッグ表示の窓が重ならないように使う）
    var visibleFrame: NSRect? {
        panel.isVisible ? panel.frame : nil
    }

    func hide() {
        guard panel.isVisible else { return }
        panel.orderOut(nil)
        // 避けていたデバッグ表示をカーソルのそばへ戻す
        DeveloperOverlay.shared.relayoutIfVisible()
    }
}

/// 予測の候補の一覧（縦に並べる）。選んでいる候補は選択色で塗り、1行目の右に操作のヒントを出す。
/// クリックは非アクティブなアプリの窓でも最初の1回で届くようにする（`acceptsFirstMouse`）
private final class CandidateListView: NSView {
    private var texts: [String] = []
    private var selected: Int?
    private var onClick: ((Int) -> Void)?

    private let font = NSFont.systemFont(ofSize: NSFont.systemFontSize)
    private let hintFont = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize)
    private let hintText = "Tab"
    private let horizontalPadding: CGFloat = 8
    private let verticalPadding: CGFloat = 4
    private let rowSpacing: CGFloat = 2
    private let hintGap: CGFloat = 12

    private var rowHeight: CGFloat { ceil(font.ascender - font.descender + font.leading) + 4 }

    override var isFlipped: Bool { true }

    func update(texts: [String], selected: Int?, onClick: @escaping (Int) -> Void) {
        self.texts = texts
        self.selected = selected
        self.onClick = onClick
        needsDisplay = true
    }

    override var fittingSize: NSSize {
        let textWidth = texts.map { ($0 as NSString).size(withAttributes: [.font: font]).width }.max() ?? 0
        let hintWidth = (hintText as NSString).size(withAttributes: [.font: hintFont]).width
        let width = ceil(horizontalPadding * 2 + textWidth + hintGap + hintWidth)
        let rows = CGFloat(texts.count)
        let height = ceil(verticalPadding * 2 + rows * rowHeight + max(0, rows - 1) * rowSpacing)
        return NSSize(width: width, height: height)
    }

    private func rowRect(_ index: Int) -> NSRect {
        NSRect(x: 0, y: verticalPadding + CGFloat(index) * (rowHeight + rowSpacing),
               width: bounds.width, height: rowHeight)
    }

    override func draw(_ dirtyRect: NSRect) {
        for (index, text) in texts.enumerated() {
            let rect = rowRect(index)
            let isSelected = index == selected
            if isSelected {
                NSColor.selectedContentBackgroundColor.setFill()
                NSBezierPath(roundedRect: rect.insetBy(dx: 3, dy: 0), xRadius: 4, yRadius: 4).fill()
            }
            let attributes: [NSAttributedString.Key: Any] = [
                .font: font,
                .foregroundColor: isSelected ? NSColor.alternateSelectedControlTextColor : NSColor.labelColor,
            ]
            let size = (text as NSString).size(withAttributes: attributes)
            (text as NSString).draw(
                at: NSPoint(x: horizontalPadding, y: rect.minY + (rect.height - size.height) / 2),
                withAttributes: attributes)
            if index == 0 {
                let hintAttributes: [NSAttributedString.Key: Any] = [
                    .font: hintFont,
                    .foregroundColor: isSelected
                        ? NSColor.alternateSelectedControlTextColor : NSColor.tertiaryLabelColor,
                ]
                let hintSize = (hintText as NSString).size(withAttributes: hintAttributes)
                (hintText as NSString).draw(
                    at: NSPoint(x: bounds.width - horizontalPadding - hintSize.width,
                                y: rect.minY + (rect.height - hintSize.height) / 2),
                    withAttributes: hintAttributes)
            }
        }
    }

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        guard let index = texts.indices.first(where: { rowRect($0).contains(point) }) else { return }
        onClick?(index)
    }
}
