import Carbon.HIToolbox
import Cocoa

/// プライベートモード（既定OFF・他のMacと同期しない。2026-10-16）。
///
/// ON の間は、入力した内容を残す処理と、これまでの入力に基づいて候補を出す処理を止める:
/// - 変換の学習（覚えることも、覚えた変換に差し替えることもしない）… `LearningSettings.isActive`
/// - 変換記録 … `ConversionLogSettings.isActive`
/// - 入力履歴の記録と予測変換 … `PredictionSettings.isActive`
///
/// 各機能の設定値は書き換えない（OFF に戻せば元の動作になる）。ユーザ辞書・変換ルールはユーザが
/// 自分で登録したものなので止めない。AI変換・選択テキストのAI処理も、ユーザが操作したときだけ動くので止めない。
/// 切り替えは設定画面・入力メニュー・ショートカット（入力中かどうかによらず、iroha に届いたキーで見る）の3か所。
/// ON/OFF は端末ごとの判断なので同期しない（別のMacで OFF にしたのが同期で届いて記録が再開しないように）。
/// ショートカットと鍵のマークの表示は同期する
enum PrivateModeSettings {

    static let enabledKey = "privateMode"
    /// 切り替えのショートカット（"Ctrl+Option+P" 形式。空なら割り当てなし）
    static let shortcutKey = "privateModeHotkey"
    static let defaultShortcut = "Ctrl+Option+P"

    static var isEnabled: Bool {
        UserDefaults.standard.bool(forKey: enabledKey)
    }

    /// モードの間、カーソルの右横に鍵のマークを出すか（`PrivateModeIndicator`、既定ON・同期する）
    static let caretIndicatorKey = "privateModeCaretIndicator"

    static var showsCaretIndicator: Bool {
        UserDefaults.standard.object(forKey: caretIndicatorKey) as? Bool ?? true
    }

    static func toggle() {
        UserDefaults.standard.set(!isEnabled, forKey: enabledKey)
    }

    static var shortcutText: String {
        UserDefaults.standard.string(forKey: shortcutKey) ?? defaultShortcut
    }

    /// 切り替えに使うショートカット。書式が読めないもの・Cmd/Ctrl/Option を含まないもの
    /// （Shift+英字だけだと普通の入力を奪う）は割り当てなしとして扱う
    static var shortcut: AICommitShortcut? {
        let shortcut = AICommitShortcut(rawValue: shortcutText)
        return isUsable(shortcut) ? shortcut : nil
    }

    static func isUsable(_ shortcut: AICommitShortcut) -> Bool {
        guard let parsed = GlobalShortcut.parse(shortcut.rawValue) else { return false }
        return parsed.carbonModifiers & UInt32(cmdKey | controlKey | optionKey) != 0
    }

    /// 入力メニューの項目に添えるキーの表示（キーが1文字のときだけ。それ以外は出さない）
    static var menuKeyEquivalent: (key: String, modifiers: NSEvent.ModifierFlags)? {
        guard let shortcut, let parsed = GlobalShortcut.parse(shortcut.rawValue),
              let key = shortcut.rawValue.split(separator: "+").last?
                .trimmingCharacters(in: .whitespaces).lowercased(),
              key.count == 1 else { return nil }
        var modifiers: NSEvent.ModifierFlags = []
        if parsed.carbonModifiers & UInt32(cmdKey) != 0 { modifiers.insert(.command) }
        if parsed.carbonModifiers & UInt32(controlKey) != 0 { modifiers.insert(.control) }
        if parsed.carbonModifiers & UInt32(optionKey) != 0 { modifiers.insert(.option) }
        if parsed.carbonModifiers & UInt32(shiftKey) != 0 { modifiers.insert(.shift) }
        return (key, modifiers)
    }
}
