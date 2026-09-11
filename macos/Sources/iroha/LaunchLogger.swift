import Foundation
import IrohaCore

/// IME本体の起動・終了を `LaunchLog`（データフォルダの `logs/`）へ記録する。
///
/// 終了の経路はいくつかあるので、すべてここを通す:
/// - AppKitの通常終了（`applicationWillTerminate`）→ reason=terminate
/// - 設定変更後の再起動（`AppRestarter`）→ reason=restart
/// - アンインストール（`Uninstaller`）→ reason=uninstall
/// - SIGTERM/SIGINT/SIGHUP（install.shのpkill・ログアウト時のlaunchd）→ reason=SIGTERM など。
///   既定動作ではハンドラ無しに即死して次回 `unclean-exit` になるため、シグナルを受けて
///   記録してから `_exit` する（従来もこの経路では後片付けは走っていない）
enum LaunchLogger {

    private static let log = LaunchLog()
    private static var didRecordLaunch = false
    private static var signalSources: [DispatchSourceSignal] = []
    private static let signalQueue = DispatchQueue(label: "iroha.launchlog.signal")

    /// IMKServerを立てる本番の起動でだけ呼ぶ（--settings やセルフインストールでは呼ばない）
    static func recordLaunch() {
        let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
        let os = ProcessInfo.processInfo.operatingSystemVersion
        let osVersion = "\(os.majorVersion).\(os.minorVersion).\(os.patchVersion)"
        log.recordLaunch(version: version, osVersion: osVersion)
        didRecordLaunch = true
        installSignalHandlers()
    }

    /// 起動を記録していない（設定ウィンドウ単体起動など）ときは何もしない
    static func recordExit(reason: String) {
        guard didRecordLaunch else { return }
        didRecordLaunch = false
        log.recordExit(reason: reason)
    }

    private static func installSignalHandlers() {
        let signals: [(Int32, String)] = [(SIGTERM, "SIGTERM"), (SIGINT, "SIGINT"), (SIGHUP, "SIGHUP")]
        for (number, name) in signals {
            signal(number, SIG_IGN)
            let source = DispatchSource.makeSignalSource(signal: number, queue: signalQueue)
            source.setEventHandler {
                recordExit(reason: name)
                _exit(0)
            }
            source.resume()
            signalSources.append(source)
        }
    }
}
