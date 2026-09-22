import Cocoa

/// カーソルの近くに一行だけ出す小さなフローティングウィンドウ。
/// 予測変換・インライン補完の予測文と、打ち間違いを直したときの知らせ、
/// 句読点スタイル（Control+.）を切り替えたときの知らせに使う。
///
/// 未確定文字列（マークテキスト）には触らない。予測をマークテキストに混ぜると、検索欄の
/// インクリメンタル検索やエディタの補完が予測文まで拾ってしまい、薄い表示になるかもアプリ任せになる。
/// azooKeyの予測候補と同じく別ウィンドウにすることで、Tabを押すまでアプリのテキストは一切変わらない。
/// 打ち間違いの知らせも同じ事情で別ウィンドウにする（マークテキストの属性はアプリが無視することがあり、
/// ライブ変換中は読みのどこが直ったかを変換結果の上では示せない）。
///
/// パネルは1枚しか持たない。予測と知らせが重なって出る事故が構造的に起きないようにするため。
/// キーボードフォーカスは取らず（nonactivating）、マウスも透過する。
/// IMKのコールバックと同じくメインスレッドから使う
final class CaretPanel {
    static let shared = CaretPanel()

    private let panel: NSPanel
    private let label: NSTextField
    private let hint: NSTextField
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
    }

    /// 予測文をカーソル行の直下に出す（取り入れるキーは Tab）
    func show(_ text: String, near caretRect: NSRect) {
        let attributed = NSAttributedString(
            string: text,
            attributes: [
                .font: NSFont.systemFont(ofSize: NSFont.systemFontSize),
                .foregroundColor: NSColor.secondaryLabelColor,
            ]
        )
        show(attributed, hint: "Tab", near: caretRect)
    }

    /// 書式付きの一行をカーソル行の直下に出す。画面の下に収まらなければ行の上に出す。
    /// `hint` が nil ならヒント欄を畳む
    func show(_ text: NSAttributedString, hint hintText: String?, near caretRect: NSRect) {
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
    }

    func hide() {
        guard panel.isVisible else { return }
        panel.orderOut(nil)
    }
}
