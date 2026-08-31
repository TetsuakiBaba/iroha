import Carbon
import Cocoa
import InputMethodKit
import SwiftUI

// IMEアプリのエントリポイント。
// システムがInfo.plistのInputMethodConnectionNameを介して接続してくる。
final class AppDelegate: NSObject, NSApplicationDelegate {
    private(set) var server: IMKServer?
    private(set) var candidatesPanel: IMKCandidates?
    private var settingsWindowController: NSWindowController?

    /// 設定ウィンドウを開く（IMEはLSBackgroundOnlyなのでレベルを上げて前面に出す）
    func openSettingsWindow() {
        if settingsWindowController == nil {
            let window = NSWindow(
                contentRect: NSRect(x: 0, y: 0, width: 440, height: 680),
                styleMask: [.titled, .closable, .resizable],
                backing: .buffered,
                defer: false
            )
            window.title = "iroha 設定"
            window.contentViewController = NSHostingController(rootView: SettingsView())
            window.isReleasedWhenClosed = false
            window.level = .modalPanel
            window.center()
            settingsWindowController = NSWindowController(window: window)
        }
        settingsWindowController?.window?.level = .modalPanel
        settingsWindowController?.showWindow(nil)
        settingsWindowController?.window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // ~/Library/Input Methods の外（Downloads等）から起動された場合は
        // セルフインストールして終了する（zipを解凍してダブルクリックするだけで導入できる）
        if SelfInstaller.installIfNeeded() { return }  // installIfNeededは戻らない（_exit）

        guard let connectionName = Bundle.main.infoDictionary?["InputMethodConnectionName"] as? String else {
            NSLog("iroha: InputMethodConnectionName がInfo.plistにありません")
            return
        }
        let server = IMKServer(name: connectionName, bundleIdentifier: Bundle.main.bundleIdentifier)
        self.server = server
        if let server {
            candidatesPanel = IMKCandidates(server: server, panelType: kIMKSingleColumnScrollingCandidatePanel)
        }
        NSLog("iroha: IMKServer 起動 connection=\(connectionName) server=\(server != nil)")

        // 変換モデルが無ければバックグラウンドでダウンロード開始
        ModelDownloader.shared.startIfNeeded()

        // macOSのユーザ辞書の取り込み（設定がONのときだけ）
        UserDictionarySync.syncOnLaunchIfEnabled()
    }
}

/// プロセスを終了し、インストール済みのirohaを即座に再起動する。
/// システム任せの遅延起動（次の入力まで起動しない）だと、その間
/// 入力ソースメニューからirohaの項目が消えてユーザを不安にさせるため、
/// openで明示的に再起動して空白時間をなくす。
/// exit()ではなく_exit()なのは、ggml(llama.cpp)のC++静的デストラクタが
/// Metal解放処理でabortすることがあるため（SettingsViewの再起動ボタンと同じ理由）
enum AppRestarter {
    static func restartInstalledApp() -> Never {
        UserDefaults.standard.synchronize()
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/sh")
        // 自プロセスの終了を待ってから起動する（0.3秒で十分）
        process.arguments = ["-c", "sleep 0.3; /usr/bin/open '\(SelfInstaller.installedURL.path)'"]
        try? process.run()
        _exit(0)
    }
}

/// zip配布用のセルフインストーラ。
/// ~/Library/Input Methods 以外から起動されたら、自身をそこへコピーして
/// 入力ソース登録し、案内を表示して終了する。
enum SelfInstaller {
    static let inputMethodsDir = NSHomeDirectory() + "/Library/Input Methods"
    static let installedURL = URL(fileURLWithPath: inputMethodsDir + "/iroha.app")

    /// インストールが必要なら実行してプロセスを終了する（戻らない）。不要ならfalse
    static func installIfNeeded() -> Bool {
        // App Translocation中はRO マウント上のパスになるが、そこからのコピーは正規の手順
        let bundleURL = Bundle.main.bundleURL.resolvingSymlinksInPath()
        guard !bundleURL.path.hasPrefix(inputMethodsDir) else { return false }

        do {
            // 実行中の旧irohaを終了（自分はInput Methods外から動いているのでマッチしない）
            runCommand("/usr/bin/pkill", ["-f", "Input Methods/iroha.app/Contents/MacOS/iroha"])
            let fm = FileManager.default
            try fm.createDirectory(atPath: inputMethodsDir, withIntermediateDirectories: true)
            if fm.fileExists(atPath: installedURL.path) {
                try fm.removeItem(at: installedURL)
            }
            try fm.copyItem(at: bundleURL, to: installedURL)
            // 公証・staple済みなので不要なはずだが、二重のGatekeeper確認を避ける保険
            runCommand("/usr/bin/xattr", ["-dr", "com.apple.quarantine", installedURL.path])
            TISRegisterInputSource(installedURL as CFURL)

            showAlert(
                message: "irohaをインストールしました",
                informative: "システム設定 > キーボード > 入力ソース > 編集 > ＋ > 日本語 >"
                    + " iroha を追加してください。\n初回入力時に変換モデル（約72MB）を自動ダウンロードします。",
                showsSettingsButton: true
            )
            // インストール先のirohaを即起動して、入力ソースメニューにすぐ現れるようにする
            AppRestarter.restartInstalledApp()
        } catch {
            showAlert(
                message: "インストールに失敗しました",
                informative: "\(error.localizedDescription)\n\n手動で iroha.app を"
                    + " ~/Library/Input Methods/ にコピーしてください。",
                showsSettingsButton: false
            )
        }
        UserDefaults.standard.synchronize()
        _exit(0)
    }

    private static func runCommand(_ path: String, _ arguments: [String]) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: path)
        process.arguments = arguments
        try? process.run()
        process.waitUntilExit()
    }

    private static func showAlert(message: String, informative: String, showsSettingsButton: Bool) {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = message
        alert.informativeText = informative
        if showsSettingsButton {
            alert.addButton(withTitle: "システム設定を開く")
            alert.addButton(withTitle: "OK")
        } else {
            alert.addButton(withTitle: "OK")
        }
        alert.window.level = .modalPanel  // LSBackgroundOnlyでも前面に出す
        let response = alert.runModal()
        if showsSettingsButton, response == .alertFirstButtonReturn,
           let url = URL(string: "x-apple.systempreferences:com.apple.Keyboard-Settings.extension") {
            NSWorkspace.shared.open(url)
        }
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.run()
