import Cocoa

/// プライベートモードの間だけ、メニューバーに鍵のアイコンを出す（2026-10-16）。
///
/// 入力ソースのアイコン（あ / A）そのものに印を付けられれば一番分かりやすいが、あのアイコンは
/// Info.plist の入力モードごとに固定で、実行中に差し替える公開の手段がない。鍵付きの入力モードを
/// 別に足して切り替える方法はあるが、入力ソースの一覧が増え、入れ替えで入力メニューから iroha が
/// 消える問題（`SelfInstaller.replaceInstalledBundle` のコメント）と同じところに触れるので採らない。
/// 代わりに独立したメニューバーの項目を、モードが ON の間だけ出す。押すとモードを切れる。
///
/// 設定画面を別プロセス（`--settings`）で開いて切り替えた場合も追えるよう、通知ではなく
/// UserDefaults のキーを KVO で見る（別プロセスの書き込みも cfprefsd 経由で届く）
final class PrivateModeStatusItem: NSObject {

    static let shared = PrivateModeStatusItem()

    private var statusItem: NSStatusItem?
    private var observation: NSKeyValueObservation?

    func start() {
        observation = UserDefaults.standard.observe(\.privateMode, options: [.initial, .new]) { [weak self] _, _ in
            DispatchQueue.main.async { self?.update() }
        }
    }

    private func update() {
        guard PrivateModeSettings.isEnabled else {
            if let statusItem { NSStatusBar.system.removeStatusItem(statusItem) }
            statusItem = nil
            return
        }
        guard statusItem == nil else { return }
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let button = item.button {
            let image = NSImage(systemSymbolName: "lock.fill", accessibilityDescription: "iroha プライベートモード")
            image?.isTemplate = true
            button.image = image
            button.toolTip = "iroha: プライベートモード（学習・記録・予測を止めています）"
        }
        item.menu = makeMenu()
        statusItem = item
    }

    private func makeMenu() -> NSMenu {
        let menu = NSMenu()
        let title = NSMenuItem(title: "iroha: プライベートモード", action: nil, keyEquivalent: "")
        title.isEnabled = false
        menu.addItem(title)
        let detail = NSMenuItem(title: "学習・変換記録・入力履歴と予測変換を止めています", action: nil, keyEquivalent: "")
        detail.isEnabled = false
        menu.addItem(detail)
        menu.addItem(.separator())
        let off = NSMenuItem(title: "プライベートモードを終了", action: #selector(turnOff(_:)), keyEquivalent: "")
        off.target = self
        menu.addItem(off)
        let settings = NSMenuItem(title: "設定...", action: #selector(openSettings(_:)), keyEquivalent: "")
        settings.target = self
        menu.addItem(settings)
        return menu
    }

    @objc private func turnOff(_ sender: Any?) {
        UserDefaults.standard.set(false, forKey: PrivateModeSettings.enabledKey)
    }

    @MainActor
    @objc private func openSettings(_ sender: Any?) {
        SettingsUIState.shared.selectedTab = .dictionary
        (NSApp.delegate as? AppDelegate)?.openSettingsWindow()
    }
}

extension UserDefaults {
    /// KVO 用。プロパティ名を `PrivateModeSettings.enabledKey` と同じにしてある（KVO はこの名前のキーを見る）
    @objc dynamic var privateMode: Bool {
        bool(forKey: PrivateModeSettings.enabledKey)
    }
}
