import Foundation

/// 設定画面の開発者モード（設定 > 情報、既定OFF・他のMacと同期しない）。
///
/// OFF の間は、細かな調整や開発者向けの項目を設定画面に出さない（2026-10-03）。
/// 隠すのは表示だけで、開発者モードで変えた値は OFF にしてもそのまま効く。
/// 例外はデバッグ表示（`DeveloperOverlaySettings`）で、これは開発者モードが ON のときだけ出す
enum DeveloperModeSettings {
    static let enabledKey = "developerMode"

    static var isEnabled: Bool {
        UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? false
    }
}
