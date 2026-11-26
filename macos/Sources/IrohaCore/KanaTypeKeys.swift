/// 入力中の文字の種類を変える変換（F6〜F10 と、その Control の組み合わせ）
public enum KanaTypeConversion: Sendable, Equatable, CaseIterable {
    case hiragana
    case katakana
    case halfWidthKatakana
    case fullWidthAlphanumeric
    case halfWidthAlphanumeric
}

/// 文字の種類を変える Control のキーの系列（設定 > 入力。2026-11-26）。
///
/// IME ごとに Control の組み合わせが違う。並びは Mozc のキー配列の表（`src/data/keymap/ms-ime.tsv`・`kotoeri.tsv`・
/// `atok.tsv`）の ConvertTo… / DisplayAs… の行に合わせた:
///
/// | 変換 | Windows（Microsoft IME） | macOS 標準（ことえり） | ATOK |
/// |---|---|---|---|
/// | ひらがな | Control+U | Control+J | Control+U |
/// | カタカナ | Control+I | Control+K | Control+I |
/// | 半角カタカナ | Control+O | Control+; | Control+O |
/// | 全角英数 | Control+P | Control+L | Control+P |
/// | 半角英数 | Control+T | Control+:（英語配列では Control+'） | Control+@ |
///
/// F6〜F10 はどの系列でも同じなので、ここには入れない
public enum KanaTypeKeyStyle: String, Sendable, CaseIterable {
    case windows
    case kotoeri
    case atok

    /// 既定は Windows（設定を足す前の iroha の動き）
    public static let defaultStyle = KanaTypeKeyStyle.windows

    /// Control と一緒に押したキーの文字（`charactersIgnoringModifiers`。英字は小文字）に当たる変換
    public func conversion(forControlKey characters: String) -> KanaTypeConversion? {
        switch self {
        case .windows:
            switch characters {
            case "u": return .hiragana
            case "i": return .katakana
            case "o": return .halfWidthKatakana
            case "p": return .fullWidthAlphanumeric
            case "t": return .halfWidthAlphanumeric
            default: return nil
            }
        case .kotoeri:
            switch characters {
            case "j": return .hiragana
            case "k": return .katakana
            case ";": return .halfWidthKatakana
            case "l": return .fullWidthAlphanumeric
            case ":", "'": return .halfWidthAlphanumeric
            default: return nil
            }
        case .atok:
            switch characters {
            case "u": return .hiragana
            case "i": return .katakana
            case "o": return .halfWidthKatakana
            case "p": return .fullWidthAlphanumeric
            case "@": return .halfWidthAlphanumeric
            default: return nil
            }
        }
    }
}
