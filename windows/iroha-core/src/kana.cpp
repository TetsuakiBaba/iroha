#include "iroha/kana.h"

namespace iroha {

std::u32string HiraganaToKatakana(const std::u32string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (char32_t cp : s) {
        if (cp >= 0x3041 && cp <= 0x3096) {
            out.push_back(cp + 0x60);
        } else {
            out.push_back(cp);
        }
    }
    return out;
}

std::u32string KatakanaToHiragana(const std::u32string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (char32_t cp : s) {
        if (cp >= 0x30A1 && cp <= 0x30F6) {
            out.push_back(cp - 0x60);
        } else {
            out.push_back(cp);
        }
    }
    return out;
}

} // namespace iroha
