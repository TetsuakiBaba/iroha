import Foundation

/// 区切り記号で囲んで英字を入力する設定（開発者モードの項目、既定OFF・他のMacと同期しない。2026-10-04）。
///
/// かな入力のまま、開始の記号（既定「_」）を打つと英字入力になり、終わりの記号（既定「_」）を打つまで
/// 打ったとおりの英字が入る（大文字・小文字は Shift で打ち分ける。Shift+スペースで英字の中にスペースを入れる）。
/// 英字は確定せず未確定文字列の固定部分になるので、「今日は_iPhone_を使う」を1回の確定で入れられる。
/// 開始の直後に終わりの記号を打つと（既定なら「__」）、開始の記号そのものをかな入力に入れる。
/// 中身は Shift+英字の英字入力（`IrohaInputController.alphabetRun`）と同じ仕組みで、続く条件だけが違う
enum DelimitedAlphabetSettings {

    static let enabledKey = "delimitedAlphabet"
    static let startKey = "delimitedAlphabetStart"
    static let endKey = "delimitedAlphabetEnd"

    static let defaultSymbol = "_"

    static var isEnabled: Bool {
        UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? false
    }

    /// 開始の記号（設定が使えない値なら既定）
    static var start: Character { symbol(forKey: startKey) }
    /// 終わりの記号（設定が使えない値なら既定）
    static var end: Character { symbol(forKey: endKey) }

    /// 区切りに使える記号か。英字・数字はローマ字や英字そのものと区別できないので使えない。
    /// キーで直接打てる ASCII の記号 1 文字に限る
    static func isValid(_ text: String) -> Bool {
        guard text.count == 1, let character = text.first, let scalar = character.unicodeScalars.first,
              character.unicodeScalars.count == 1 else { return false }
        return (0x21...0x7E).contains(scalar.value) && !character.isLetter && !character.isNumber
    }

    private static func symbol(forKey key: String) -> Character {
        guard let text = UserDefaults.standard.string(forKey: key), isValid(text), let character = text.first
        else { return Character(defaultSymbol) }
        return character
    }
}
