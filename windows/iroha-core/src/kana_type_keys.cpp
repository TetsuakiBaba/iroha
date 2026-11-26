#include "iroha/kana_type_keys.h"

namespace iroha {

KanaTypeKeyStyle ParseKanaTypeKeyStyle(const std::string& value) {
    if (value == "kotoeri") return KanaTypeKeyStyle::Kotoeri;
    if (value == "atok") return KanaTypeKeyStyle::Atok;
    return kDefaultKanaTypeKeyStyle;
}

std::string KanaTypeKeyStyleName(KanaTypeKeyStyle style) {
    switch (style) {
    case KanaTypeKeyStyle::Kotoeri: return "kotoeri";
    case KanaTypeKeyStyle::Atok: return "atok";
    case KanaTypeKeyStyle::Windows: break;
    }
    return "windows";
}

std::optional<KanaTypeConversion> KanaTypeConversionForControlKey(KanaTypeKeyStyle style, char32_t character) {
    switch (style) {
    case KanaTypeKeyStyle::Windows:
        switch (character) {
        case U'u': return KanaTypeConversion::Hiragana;
        case U'i': return KanaTypeConversion::Katakana;
        case U'o': return KanaTypeConversion::HalfWidthKatakana;
        case U'p': return KanaTypeConversion::FullWidthAlphanumeric;
        case U't': return KanaTypeConversion::HalfWidthAlphanumeric;
        default: return std::nullopt;
        }
    case KanaTypeKeyStyle::Kotoeri:
        switch (character) {
        case U'j': return KanaTypeConversion::Hiragana;
        case U'k': return KanaTypeConversion::Katakana;
        case U';': return KanaTypeConversion::HalfWidthKatakana;
        case U'l': return KanaTypeConversion::FullWidthAlphanumeric;
        case U':': case U'\'': return KanaTypeConversion::HalfWidthAlphanumeric;
        default: return std::nullopt;
        }
    case KanaTypeKeyStyle::Atok:
        switch (character) {
        case U'u': return KanaTypeConversion::Hiragana;
        case U'i': return KanaTypeConversion::Katakana;
        case U'o': return KanaTypeConversion::HalfWidthKatakana;
        case U'p': return KanaTypeConversion::FullWidthAlphanumeric;
        case U'@': return KanaTypeConversion::HalfWidthAlphanumeric;
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<KanaTypeConversion> KanaTypeConversionForControlKey(KanaTypeKeyStyle style, char32_t character,
                                                                  bool shift) {
    if (shift && character != U'@') return std::nullopt;
    return KanaTypeConversionForControlKey(style, character);
}

} // namespace iroha
