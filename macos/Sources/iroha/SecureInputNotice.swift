import Carbon.HIToolbox
import Cocoa
import IOKit
import IrohaCore

/// Secure Input で iroha を選べないときの知らせ（設定 > 入力、既定ON・他のMacと同期する。2026-10-31）
enum SecureInputNoticeSettings {
    static let enabledKey = "secureInputNotice"

    static var isEnabled: Bool {
        UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? true
    }
}

/// 別のアプリが Secure Input（パスワード入力の保護）を有効にしたままのとき、入力メニューで iroha を選べなくなったことを
/// マウスポインタの近くの小窓で知らせる（2026-10-31）。
///
/// macOS は Secure Input の間、前面のアプリが切り替わると入力メニューで iroha を灰色にする（Chrome がパスワード画面を
/// 離れても Secure Input を解除せず、終了するまで続いた）。灰色にする条件は分からなかった（置き場所・App Sandbox・
/// プロビジョニングプロファイル・公証・入力メソッドとしての動きを azooKey に合わせても変わらず、azooKey は選べる）。
/// 灰色になるのは前面のアプリが切り替わったときなので、その 2 秒後に確かめる（定期的には確かめない）。
/// 知らせるかは IrohaCore の `SecureInputNoticeRule` が決める。灰色の間 iroha は入力に使われておらず、
/// カーソルの位置は分からないので、小窓はマウスポインタの近くに出る（`TapInputFeedback`）
final class SecureInputNotice {
    static let shared = SecureInputNotice()

    /// 前面のアプリが切り替わってから確かめるまで（パスワード欄を離れた直後の、解除が遅れているだけの間は出さない）
    private static let delay: TimeInterval = 2
    private static let duration: TimeInterval = 8

    private var rule = SecureInputNoticeRule()
    private var observer: NSObjectProtocol?
    private var pending: DispatchWorkItem?

    private init() {}

    func start() {
        guard observer == nil else { return }
        observer = NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didActivateApplicationNotification, object: nil, queue: .main
        ) { [weak self] _ in
            self?.scheduleCheck()
        }
    }

    private func scheduleCheck() {
        pending?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.check() }
        pending = work
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.delay, execute: work)
    }

    private func check() {
        guard SecureInputNoticeSettings.isEnabled else { return }
        let observation = SecureInputNoticeRule.Observation(
            isSecureInputEnabled: IsSecureEventInputEnabled(),
            holder: Self.holderPID(),
            frontmost: NSWorkspace.shared.frontmostApplication?.processIdentifier,
            own: ProcessInfo.processInfo.processIdentifier)
        guard rule.shouldNotify(observation), let holder = observation.holder else { return }
        let name = NSRunningApplication(processIdentifier: holder)?.localizedName
        TapInputFeedback.shared.show(SecureInputNoticeRule.message(holderName: name), duration: Self.duration)
    }

    /// Secure Input を持っているプロセス（`ioreg -l | grep kCGSSessionSecureInputPID` と同じ値）。
    /// 画面を持たないプロセスが有効にしたときは、それを起動したアプリの値になることがある
    static func holderPID() -> pid_t? {
        let root = IORegistryGetRootEntry(kIOMainPortDefault)
        defer { IOObjectRelease(root) }
        guard let users = IORegistryEntryCreateCFProperty(
            root, "IOConsoleUsers" as CFString, kCFAllocatorDefault, 0
        )?.takeRetainedValue() as? [[String: Any]] else { return nil }
        for user in users {
            if let pid = user["kCGSSessionSecureInputPID"] as? Int { return pid_t(pid) }
        }
        return nil
    }
}
