import AppKit

/// 他アプリでのマウス操作（クリック・スクロール）とアプリ切替を監視して、カーソル位置に出している
/// フローティングウィンドウ（候補ウィンドウ・予測/補完の小窓）を閉じる合図を出す。
///
/// 確定後の補完の小窓は未確定文字列を持たないため、同じ文書内で別の場所をクリックしても
/// アプリからIMEへは何も通知されず、キーを押すまで残ってしまう。候補ウィンドウも
/// commitCompositionを呼ばないアプリでは残る。そこでIME側から自前で操作を検出する。
///
/// マウス系のグローバル監視はアクセシビリティ権限なしで届く（権限が要るのはキー入力の監視のみ）。
/// 自プロセスのウィンドウ（候補ウィンドウ・設定画面）への操作はグローバル監視には届かないので、
/// 候補をクリックして選ぶ操作は妨げない。
/// IMKのコールバックと同じくメインスレッドから使う（合図もメインスレッドで出す）
final class PointerActivityMonitor {
    enum Event {
        /// 他アプリのどこかをクリックした（左・右・中ボタン）
        case mouseDown
        /// 他アプリでスクロールした（カーソル行が動くので窓の位置が合わなくなる）
        case scroll
        /// 前面のアプリが変わった、または操作スペースが切り替わった
        case focusChanged
    }

    static let shared = PointerActivityMonitor()

    /// 合図の受け手。アクティブな入力コントローラが activateServer で差し替える
    var handler: ((Event) -> Void)?

    private var eventMonitor: Any?
    private var observers: [NSObjectProtocol] = []

    private init() {}

    func start() {
        guard eventMonitor == nil else { return }
        eventMonitor = NSEvent.addGlobalMonitorForEvents(
            matching: [.leftMouseDown, .rightMouseDown, .otherMouseDown, .scrollWheel]
        ) { [weak self] event in
            let kind: Event = event.type == .scrollWheel ? .scroll : .mouseDown
            Task { @MainActor [weak self] in self?.handler?(kind) }
        }
        let center = NSWorkspace.shared.notificationCenter
        observers.append(center.addObserver(
            forName: NSWorkspace.didActivateApplicationNotification, object: nil, queue: .main
        ) { [weak self] notification in
            // 自分（設定ウィンドウ等）が前面に来たときは対象外。IMEの窓は入力先アプリの上に出ているだけ
            let app = notification.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication
            guard app?.processIdentifier != ProcessInfo.processInfo.processIdentifier else { return }
            Task { @MainActor [weak self] in self?.handler?(.focusChanged) }
        })
        observers.append(center.addObserver(
            forName: NSWorkspace.activeSpaceDidChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in
            Task { @MainActor [weak self] in self?.handler?(.focusChanged) }
        })
    }
}
