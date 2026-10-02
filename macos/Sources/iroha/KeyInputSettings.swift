import Foundation

/// キーで入る文字の設定（¥キーの文字・スペースの幅）。どちらも既定は設定を足す前と同じ動き
enum KeyInputSettings {

    /// JISキーボードの ¥ キーで入れる文字（"¥" または "\"）。Option+¥ ではもう一方が入る
    static let yenKeyCharacterKey = "yenKeyCharacter"
    static let yen = "¥"
    static let backslash = "\\"

    static var yenKeyCharacter: String {
        UserDefaults.standard.string(forKey: yenKeyCharacterKey) == backslash ? backslash : yen
    }

    /// ¥ キーで入れる文字。Option を押していればもう一方
    static func yenKeyText(option: Bool) -> String {
        let base = yenKeyCharacter
        guard option else { return base }
        return base == yen ? backslash : yen
    }

    /// ひらがなモードでも、入力していないときのスペースを半角にする（既定ON）。
    /// OFF なら全角スペースを入れ、Shift+スペースで半角を入れる
    static let alwaysHalfWidthSpaceKey = "alwaysHalfWidthSpace"

    static var alwaysHalfWidthSpace: Bool {
        UserDefaults.standard.object(forKey: alwaysHalfWidthSpaceKey) as? Bool ?? true
    }
}
