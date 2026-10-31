import ApplicationServices
import Carbon.HIToolbox
import Cocoa
import IrohaCore

/// タップ入力の設定（設定 > 入力 > タップ入力。開発者モードで加速度センサを利用するときだけ出す・既定OFF・
/// 他のMacと同期しない（センサの有無も叩き方も機械ごとのため）。2026-10-24）
enum TapInputSettings {
    static let enabledKey = "tapInputEnabled"
    /// ⌘ を押しながら本体をダブルタップしたときに送るキーショートカット（"Option+Return" 形式。空なら送らない）
    static let doubleTapShortcutKey = "tapInputDoubleTapShortcut"

    static var isEnabled: Bool {
        AccelerometerSettings.isEnabled && UserDefaults.standard.bool(forKey: enabledKey)
    }

    static var doubleTapShortcut: GlobalShortcut? {
        UserDefaults.standard.string(forKey: doubleTapShortcutKey).flatMap(GlobalShortcut.parse)
    }
}

/// ⌘ を押しながら本体を指でダブルタップしたら、設定したキーショートカットを送る（タップ入力。試験的。2026-10-24）。
///
/// タップは加速度センサ（`KeystrokeAccelerometer`）の揺れから IrohaCore の `TapGestureDetector` が数える。
/// ⌘ を押してから離すまでを 1 回とし、離した時点で数えるので、3 回目が来ないことを待つ時間は要らない。
/// ⌘ を押している間に他のキー・クリック・他の修飾キーが入ったら（⌘C などを使った）数えない。
/// 修飾キーなしでは手を置き直しただけの揺れもタップに見えるので、⌘ を押している間だけ数える
/// （実験 `experiments/keystroke-force/` で、⌘ を押していた約 113 秒にダブルタップ以上の誤検出は 0）。
///
/// キー・クリック・修飾キーの時刻は、⌘ つきのキーなど IME に届かないものもあるので、OS の「最後の入力からの経過時間」
/// （`CGEventSource.secondsSinceLastEventType`。権限は要らない）を 20ms ごとに読んで作る。
/// 送るのは合成したキーイベントなので、アクセシビリティの許可が要る（選択テキストのAI編集と同じ）
final class TapInput {
    static let shared = TapInput()

    /// タップを数えたとき（設定画面の「直近の検出」）。メインスレッドで送る
    static let detectedNotification = Notification.Name("iroha.tapInput.detected")

    struct Detection {
        var count: Int
        var date: Date
        /// 送ったショートカット（送らなかったら nil）
        var sentShortcut: String?
        var accessibilityGranted: Bool

        /// カーソルの近くの表示と設定画面の「直近の検出」に出す文
        var message: String {
            guard count == 2 else { return "タップ \(count) 回（割り当てなし）" }
            if let sentShortcut { return "⌘ + ダブルタップ → \(sentShortcut)" }
            if !accessibilityGranted { return "⌘ + ダブルタップ（アクセシビリティが未許可）" }
            return "⌘ + ダブルタップ（キーが未設定）"
        }
    }

    /// 直近に数えた結果（メインスレッド）
    private(set) static var lastDetection: Detection?

    private var started = false
    private var observing = false

    private init() {}

    /// 起動時に 1 回呼ぶ（main.swift）。以後は設定が変わるたびにセンサとタップの検出を合わせる
    func start() {
        guard !started else { return }
        started = true
        sync()
        NotificationCenter.default.addObserver(
            forName: UserDefaults.didChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in self?.sync() }
    }

    private func sync() {
        KeystrokeAccelerometer.shared.syncWithSettings()
        let wanted = TapInputSettings.isEnabled
        guard wanted != observing else { return }
        observing = wanted
        // ON にするたびに数える状態を作り直す（状態はセンサのスレッドだけで触る）
        let session = wanted ? TapSession() : nil
        KeystrokeAccelerometer.shared.setSampleObserver(session.map { session in { session.receive(t: $0, magnitude: $1) } })
    }

    /// タップを数えた（メインスレッド）
    fileprivate static func didDetect(count: Int) {
        guard TapInputSettings.isEnabled else { return }
        let trusted = AXIsProcessTrusted()
        var sent: String?
        if count == 2, let shortcut = TapInputSettings.doubleTapShortcut, trusted {
            sent = shortcut.rawValue
        }
        let detection = Detection(count: count, date: Date(), sentShortcut: sent, accessibilityGranted: trusted)
        lastDetection = detection
        // 1 回だけのときは出さない（⌘ を押すたびに、手の揺れで出ることがあるため）。
        // 表示の位置はキーを送る前に取る（送ったキーでカーソルが動くことがあるため）
        if count >= 2 { TapInputFeedback.shared.show(detection.message) }
        if let sent, let shortcut = GlobalShortcut.parse(sent) { post(shortcut) }
        NotificationCenter.default.post(name: detectedNotification, object: nil)
    }

    /// キーを押して離したのと同じイベントを送る（`SelectionService` の ⌘C・⌘V と同じ送り方）
    private static func post(_ shortcut: GlobalShortcut) {
        var flags: CGEventFlags = []
        if shortcut.carbonModifiers & UInt32(cmdKey) != 0 { flags.insert(.maskCommand) }
        if shortcut.carbonModifiers & UInt32(controlKey) != 0 { flags.insert(.maskControl) }
        if shortcut.carbonModifiers & UInt32(optionKey) != 0 { flags.insert(.maskAlternate) }
        if shortcut.carbonModifiers & UInt32(shiftKey) != 0 { flags.insert(.maskShift) }
        let key = CGKeyCode(shortcut.keyCode)
        guard let down = CGEvent(keyboardEventSource: nil, virtualKey: key, keyDown: true),
              let up = CGEvent(keyboardEventSource: nil, virtualKey: key, keyDown: false) else { return }
        down.flags = flags
        up.flags = flags
        down.post(tap: .cghidEventTap)
        up.post(tap: .cghidEventTap)
    }
}

/// センサのスレッドでタップを数える。⌘ の押し離しと他の入力を、揺れを受け取るついでに 20ms ごとに読む
private final class TapSession {
    private var detector = TapGestureDetector()
    private var sampleCount = 0
    private var lastInputTimes: [UInt32: Double] = [:]
    /// ⌘ を押し始めた時刻（押していなければ nil）
    private var commandDown: Double?
    /// ⌘ を押している間に他のキー・クリック・修飾キーが入った
    private var commandSpoiled = false
    /// ⌘ を押していた間（離したあと、判定が追いつくのを待つ）
    private var pending: (start: Double, end: Double)?

    /// 原因として見る入力（修飾キーは `flagsChanged` で別に読む）
    private static let polledTypes: [CGEventType] = [
        .keyDown, .keyUp, .leftMouseDown, .leftMouseUp, .rightMouseDown, .rightMouseUp, .otherMouseDown, .otherMouseUp,
    ]
    /// 約 800 件/秒のうち 16 件ごと（20ms ごと）に入力を読む
    private static let pollInterval = 16
    /// ⌘ をこれより長く押していたら数えない（叩くつもりの長さではない）
    private static let maximumHold = 3.0

    func receive(t: Double, magnitude: Float) {
        sampleCount += 1
        if sampleCount % Self.pollInterval == 0 { pollInputs() }
        detector.addSample(t: t, magnitude: magnitude)
        if let window = pending, detector.processedUntil >= window.end {
            pending = nil
            if let count = detector.tapCount(from: window.start, to: window.end), count > 0 {
                DispatchQueue.main.async { TapInput.didDetect(count: count) }
            }
        }
    }

    private func pollInputs() {
        let now = machSeconds(mach_absolute_time())
        for type in Self.polledTypes {
            guard let t = newInputTime(type, now: now) else { continue }
            if let down = commandDown, t >= down { commandSpoiled = true }
        }
        let flagsTime = newInputTime(.flagsChanged, now: now)
        let flags = CGEventSource.flagsState(.combinedSessionState)
        let command = flags.contains(.maskCommand)
        let others = !flags.intersection([.maskShift, .maskControl, .maskAlternate]).isEmpty
        if let down = commandDown {
            if others { commandSpoiled = true }
            if !command {
                let up = flagsTime ?? now
                if !commandSpoiled, up - down <= Self.maximumHold { pending = (down, up) }
                commandDown = nil
            }
        } else if command {
            commandDown = flagsTime ?? now
            commandSpoiled = others
        }
    }

    /// 前に読んだときから、その種類の入力が新しく入っていればその時刻（検出の「原因」にも足す）
    private func newInputTime(_ type: CGEventType, now: Double) -> Double? {
        let t = now - CGEventSource.secondsSinceLastEventType(.combinedSessionState, eventType: type)
        if let last = lastInputTimes[type.rawValue], abs(last - t) < 0.002 { return nil }
        let isFirst = lastInputTimes[type.rawValue] == nil
        lastInputTimes[type.rawValue] = t
        detector.addInput(t: t)
        // 最初に読んだ値は ON にする前の入力なので、⌘ の押し離しの判定には使わない
        return isFirst ? nil : t
    }
}

/// タップ入力を数えたことを、カーソルの近くに 1 秒ほど出す小窓（「⌘ + ダブルタップ → Ctrl+Space」など）。
///
/// 予測変換・打ち間違いの知らせの `CaretPanel` とは別の窓にする（`CaretPanel` は「訂正の小窓が出ている ⇒
/// Backspace で取り消せる」を 1 枚で保っているので、相乗りするとその約束が崩れる）。
/// カーソルの位置は iroha で入力しているコントローラから取り（`attach`）、取れなければマウスポインタの近くに出す。
/// キーボードフォーカスは取らず、マウスも透過する。メインスレッドから使う
final class TapInputFeedback {
    static let shared = TapInputFeedback()

    private let panel: NSPanel
    private let label: NSTextField
    private let padding = NSEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
    /// カーソル行とウィンドウの間隔
    private let caretGap: CGFloat = 2
    static let duration: TimeInterval = 1.2
    private var hideWork: DispatchWorkItem?
    private weak var caretOwner: AnyObject?
    private var caretRect: (() -> NSRect?)?

    private init() {
        panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 120, height: 28),
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
    }

    /// 入力を受け持つコントローラが、カーソルの位置の問い合わせ方を渡す（有効になったとき）
    func attach(_ owner: AnyObject, caretRect: @escaping () -> NSRect?) {
        caretOwner = owner
        self.caretRect = caretRect
    }

    /// 無効になったとき。別のコントローラが先に attach していたら何もしない
    func detach(_ owner: AnyObject) {
        guard caretOwner === owner else { return }
        caretOwner = nil
        caretRect = nil
    }

    /// Secure Input の知らせ（`SecureInputNotice`）もこの小窓を使う（文が長いので長めに出す）
    func show(_ text: String, duration: TimeInterval = TapInputFeedback.duration) {
        label.stringValue = text
        label.sizeToFit()
        let size = NSSize(width: padding.left + label.frame.width + padding.right,
                          height: padding.top + label.frame.height + padding.bottom)
        label.frame.origin = NSPoint(x: padding.left, y: padding.bottom)
        let anchor = (caretOwner != nil ? caretRect?() : nil)
            ?? NSRect(origin: NSEvent.mouseLocation, size: NSSize(width: 0, height: 16))
        panel.setFrame(NSRect(origin: origin(for: size, near: anchor), size: size), display: true)
        panel.orderFrontRegardless()

        hideWork?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.panel.orderOut(nil) }
        hideWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + duration, execute: work)
    }

    /// カーソル行の下。予測・訂正の小窓と重なるならその下、画面の下に収まらなければ行の上
    private func origin(for size: NSSize, near anchor: NSRect) -> NSPoint {
        var origin = NSPoint(x: anchor.minX, y: anchor.minY - caretGap - size.height)
        if let other = CaretPanel.shared.visibleFrame,
           NSRect(origin: origin, size: size).intersects(other) {
            origin.y = other.minY - caretGap - size.height
        }
        let screen = NSScreen.screens.first { $0.frame.contains(anchor.origin) } ?? NSScreen.main
        if let visible = screen?.visibleFrame {
            if origin.y < visible.minY { origin.y = anchor.maxY + caretGap }
            origin.x = min(max(origin.x, visible.minX), max(visible.minX, visible.maxX - size.width))
        }
        return origin
    }
}

