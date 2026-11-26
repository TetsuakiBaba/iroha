#include "iroha/kana_type_keys.h"

namespace iroha {

KanaTypeKeyStyle ParseKanaTypeKeyStyle(const std::string& value) {
    if (value == "kotoeri") return KanaTypeKeyStyle::Kotoeri;
    if (value == "atok") return KanaTypeKeyStyle::Atok;
    return kDefaultKanaTypeKeyStyle;
}

const char* KanaTypeKeyStyleName(KanaTypeKeyStyle style) {
    switch (style) {
        case KanaTypeKeyStyle::Kotoeri: return "kotoeri";
        case KanaTypeKeyStyle::Atok: return "atok";
        case KanaTypeKeyStyle::Windows: break;
    }
    return "windows";
}

std::optional<KanaTypeConversion> KanaTypeConversionForControlKey(KanaTypeKeyStyle style, char32_t character) {
    using C = KanaTypeConversion;
    switch (style) {
        case KanaTypeKeyStyle::Windows:
            switch (character) {
                case U'u': return C::Hiragana;
                case U'i': return C::Katakana;
                case U'o': return C::HalfWidthKatakana;
                case U'p': return C::FullWidthAlphanumeric;
                case U't': return C::HalfWidthAlphanumeric;
                default: return std::nullopt;
            }
        case KanaTypeKeyStyle::Kotoeri:
            switch (character) {
                case U'j': return C::Hiragana;
                case U'k': return C::Katakana;
                case U';': return C::HalfWidthKatakana;
                case U'l': return C::FullWidthAlphanumeric;
                case U':':
                case U'\'': return C::HalfWidthAlphanumeric;
                default: return std::nullopt;
            }
        case KanaTypeKeyStyle::Atok:
            switch (character) {
                case U'u': return C::Hiragana;
                case U'i': return C::Katakana;
                case U'o': return C::HalfWidthKatakana;
                case U'p': return C::FullWidthAlphanumeric;
                case U'@': return C::HalfWidthAlphanumeric;
                default: return std::nullopt;
            }
    }
    return std::nullopt;
}

} // namespace iroha
