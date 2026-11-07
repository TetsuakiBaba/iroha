import Foundation
import IrohaCore

/// 「AIで直す」の設定（開発者モードの項目・既定OFF・同期する。2026-11-07）。
///
/// ライブ変換の結果に誤変換の疑いがあるとき（`AIFixRule`）、入力が止まった時点で未確定文字列の右上に
/// 「AIで直す」の窓（`AIFixBadge`）を出す。窓をクリックするかショートカットを押すと、文脈・読み・変換結果を
/// AI変換と同じ接続先（`TranslationService`）に送り、返ってきた案で未確定文字列を差し替える。
/// AI に送るのは押したときだけ。プライベートモードの間は窓を出さない。
/// 判定にはモデル単体の変換（`ZenzEngine` / `MLXConversionEngine` の `convertWithConfidence`）を使う
enum AIFixSettings {

    static let enabledKey = "aiFixEnabled"
    /// 疑う閾値（文字ごとのマージンの最小値。nat）
    static let thresholdKey = "aiFixThreshold"
    /// ショートカット（"Ctrl+Option+A" 形式。窓が出ていなくても入力中なら効く。空ならクリックだけ）
    static let shortcutKey = "aiFixHotkey"
    static let defaultShortcut = "Ctrl+Option+A"

    static var isEnabled: Bool {
        UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? false
    }

    /// 今働かせるか（プライベートモードの間は外部に送る機能を止める）
    static var isActive: Bool {
        isEnabled && !PrivateModeSettings.isEnabled
    }

    static var threshold: Float {
        let value = UserDefaults.standard.object(forKey: thresholdKey) as? Double
        guard let value, value > 0 else { return AIFixRule.defaultThreshold }
        return Float(value)
    }

    static var shortcutText: String {
        UserDefaults.standard.string(forKey: shortcutKey) ?? defaultShortcut
    }

    /// 書式が読めないもの・Cmd/Ctrl/Option を含まないものは割り当てなし
    static var shortcut: AICommitShortcut? {
        let shortcut = AICommitShortcut(rawValue: shortcutText)
        return PrivateModeSettings.isUsable(shortcut) ? shortcut : nil
    }
}
