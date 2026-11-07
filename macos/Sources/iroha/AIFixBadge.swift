import Cocoa

/// 「AIで直す」の窓（`AIFixSettings`）。未確定文字列の末尾の上（右上）に出す。
///
/// 予測変換の候補・打ち間違いの訂正の知らせは行の下に出る `CaretPanel` なので、それとは別の窓にして
/// 重ならないようにする（右横のプライベートモードの鍵のマークとも重ならない）。
/// キーボードフォーカスは取らない（nonactivating）。クリックは「AIで直す」を出している間だけ受け、
/// 非アクティブな窓でも最初の 1 回で届くようにする（`acceptsFirstMouse`。予測の候補と同じ）。
/// IMK のコールバックと同じくメインスレッドから使う
final class AIFixBadge {
    static let shared = AIFixBadge()

    private let panel: NSPanel
    private let label: NSTextField
    private let hint: NSTextField
    private let clickView = ClickView()
    private let padding = NSEdgeInsets(top: 3, left: 8, bottom: 3, right: 8)
    private let hintGap: CGFloat = 6
    /// 行と窓の間隔
    private let caretGap: CGFloat = 2

    private init() {
        panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 100, height: 22),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: true
        )
        panel.level = .popUpMenu
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.ignoresMouseEvents = true
        panel.hidesOnDeactivate = false
        panel.isReleasedWhenClosed = false
        panel.animationBehavior = .none
        panel.becomesKeyOnlyIfNeeded = true

        let background = NSVisualEffectView()
        background.material = .popover
        background.blendingMode = .behindWindow
        background.state = .active
        background.wantsLayer = true
        background.layer?.cornerRadius = 6
        background.layer?.masksToBounds = true
        panel.contentView = background

        label = NSTextField(labelWithString: "")
        label.font = .systemFont(ofSize: NSFont.smallSystemFontSize, weight: .medium)
        label.lineBreakMode = .byClipping
        background.addSubview(label)

        hint = NSTextField(labelWithString: "")
        hint.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        hint.textColor = .tertiaryLabelColor
        background.addSubview(hint)

        background.addSubview(clickView)
    }

    var isVisible: Bool { panel.isVisible }

    /// 「AIで直す」を出す。クリックすると `onClick` が呼ばれる
    func showOffer(near caretRect: NSRect, shortcut: String?, onClick: @escaping () -> Void) {
        label.textColor = .controlAccentColor
        show(text: "AIで直す", hint: shortcut, near: caretRect)
        clickView.onClick = onClick
        panel.ignoresMouseEvents = false
    }

    /// 知らせを出す（AI の確認中・結果・失敗）。クリックは受けない
    func showMessage(_ text: String, near caretRect: NSRect) {
        label.textColor = .secondaryLabelColor
        show(text: text, hint: nil, near: caretRect)
        clickView.onClick = nil
        panel.ignoresMouseEvents = true
    }

    func hide() {
        clickView.onClick = nil
        guard panel.isVisible else { return }
        panel.orderOut(nil)
    }

    private func show(text: String, hint hintText: String?, near caretRect: NSRect) {
        label.stringValue = text
        label.sizeToFit()
        hint.stringValue = hintText ?? ""
        hint.sizeToFit()
        hint.isHidden = hintText == nil
        let gap = hintText == nil ? 0 : hintGap
        let hintWidth = hintText == nil ? 0 : hint.frame.width
        let width = ceil(padding.left + label.frame.width + gap + hintWidth + padding.right)
        let height = ceil(max(label.frame.height, hintText == nil ? 0 : hint.frame.height) + padding.top + padding.bottom)
        label.frame.origin = NSPoint(x: padding.left, y: (height - label.frame.height) / 2)
        hint.frame.origin = NSPoint(x: padding.left + label.frame.width + gap, y: (height - hint.frame.height) / 2)
        clickView.frame = NSRect(x: 0, y: 0, width: width, height: height)
        place(size: NSSize(width: width, height: height), near: caretRect)
    }

    /// 行の上、カーソルの少し右に置く。画面の上に収まらなければ行の下に置く
    private func place(size: NSSize, near caretRect: NSRect) {
        let screen = NSScreen.screens.first { $0.frame.contains(caretRect.origin) } ?? NSScreen.main
        var origin = NSPoint(x: caretRect.minX + 4, y: caretRect.maxY + caretGap)
        if let visible = screen?.visibleFrame {
            if origin.y + size.height > visible.maxY {
                origin.y = caretRect.minY - caretGap - size.height
            }
            origin.x = min(max(origin.x, visible.minX), max(visible.minX, visible.maxX - size.width))
        }
        panel.setFrame(NSRect(origin: origin, size: size), display: true)
        panel.orderFrontRegardless()
    }
}

/// 窓全体でクリックを受ける透明な面
private final class ClickView: NSView {
    var onClick: (() -> Void)?

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func mouseDown(with event: NSEvent) {
        onClick?()
    }

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: .pointingHand)
    }
}
