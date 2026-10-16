import Cocoa

/// プライベートモードの間、カーソルの右横に出す鍵のマーク（`PrivateModeSettings.showsCaretIndicator`、既定ON）。
///
/// 切り替えたときの知らせ（カーソル下の小窓）は数秒で消えるので、モードが続いていることは
/// これで示す。入力ソースのアイコン（あ / A）は Info.plist の入力モードに固定で実行中に差し替えられず、
/// メニューバーに別のアイコンを出す方法は位置が離れていて分かりにくかった（2026-10-16 に取り下げ）。
///
/// 予測・知らせの小窓（`CaretPanel`）や候補ウィンドウはカーソル行の下に出るので、行の右横に置けば重ならない。
/// 背景や影は付けず鍵の形だけを出す（背景付きだと、カーソルの右に続く文字に重なったときにその文字を隠す）。
/// 色は白地でも黒地でも見える中間の灰色にする。
/// キーボードフォーカスもマウスも取らない。IMKのコールバックと同じくメインスレッドから使う
final class PrivateModeIndicator {
    static let shared = PrivateModeIndicator()

    private let panel: NSPanel
    private let imageView: NSImageView
    /// カーソルとマークの間隔
    private let caretGap: CGFloat = 1
    private let size: CGFloat = 14

    private init() {
        panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: size, height: size),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: true
        )
        panel.level = .popUpMenu
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.ignoresMouseEvents = true
        panel.hidesOnDeactivate = false
        panel.isReleasedWhenClosed = false
        panel.animationBehavior = .none

        let content = NSView()
        panel.contentView = content

        let configuration = NSImage.SymbolConfiguration(pointSize: 11, weight: .semibold)
        let image = NSImage(systemSymbolName: "lock.fill", accessibilityDescription: "プライベートモード")?
            .withSymbolConfiguration(configuration)
        imageView = NSImageView(image: image ?? NSImage())
        imageView.contentTintColor = .systemGray
        imageView.frame = NSRect(x: 0, y: 0, width: size, height: size)
        content.addSubview(imageView)
    }

    /// カーソル（幅0の矩形、スクリーン座標）の右横、行の高さの中央に出す。画面の右端に収まらなければ左横に出す
    func show(near caretRect: NSRect) {
        let screen = NSScreen.screens.first { $0.frame.contains(caretRect.origin) } ?? NSScreen.main
        var origin = NSPoint(x: caretRect.maxX + caretGap, y: caretRect.midY - size / 2)
        if let visible = screen?.visibleFrame, origin.x + size > visible.maxX {
            origin.x = caretRect.minX - caretGap - size
        }
        panel.setFrame(NSRect(origin: origin, size: NSSize(width: size, height: size)), display: true)
        panel.orderFrontRegardless()
    }

    func hide() {
        guard panel.isVisible else { return }
        panel.orderOut(nil)
    }
}
