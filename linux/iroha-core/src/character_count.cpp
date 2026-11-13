#include "iroha/character_count.h"

#include <string>
#include <vector>

#include "iroha/unicode.h"

namespace iroha {

namespace {

bool IsNewline(char32_t c) {
    return c == U'\n' || c == U'\r' || c == 0x0B || c == 0x0C || c == 0x85 || c == 0x2028 || c == 0x2029;
}

bool IsWhitespace(char32_t c) {
    return c == U' ' || c == U'\t' || c == 0xA0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F ||
           c == 0x205F || c == 0x1680;
}

// Unicode の Ideographic（漢字・々〆〇 など）
bool IsIdeographic(char32_t c) {
    return (c >= 0x3006 && c <= 0x3007) || (c >= 0x3021 && c <= 0x3029) || (c >= 0x3038 && c <= 0x303A) ||
           (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0x20000 && c <= 0x3134F);
}

// かな（半角カナを含む）か漢字
bool IsJapanese(char32_t c) {
    return (c >= 0x3040 && c <= 0x309F) || (c >= 0x30A0 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) ||
           (c >= 0xFF66 && c <= 0xFF9F) || IsIdeographic(c);
}

// 文字（Swift の Character.isLetter のうち、英文に出るもの。かな・漢字は呼び出し側で先に見る）
bool IsLetter(char32_t c) {
    return (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z') || (c >= 0xFF21 && c <= 0xFF3A) ||
           (c >= 0xFF41 && c <= 0xFF5A) || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
           (c >= 0x370 && c <= 0x3FF) || (c >= 0x400 && c <= 0x4FF);
}

std::u32string Number(int value) { return Utf8ToUtf32(std::to_string(value)); }

} // namespace

CharacterCount::CharacterCount(const std::u32string& text) {
    bool inWord = false;
    bool hasJapanese = false;
    bool hasLetter = false;
    for (const std::u32string& character : SplitGraphemes(text)) {
        const char32_t first = character.empty() ? 0 : character[0];
        const bool newline = IsNewline(first);
        const bool whitespace = IsWhitespace(first);
        if (newline || whitespace) {
            inWord = false;
        } else if (!inWord) {
            inWord = true;
            ++words;
        }
        for (char32_t c : character) {
            if (IsJapanese(c)) hasJapanese = true;
        }
        if (IsLetter(first) || IsJapanese(first)) hasLetter = true;
        if (newline) continue;
        ++total;
        if (!whitespace) ++withoutWhitespace;
    }
    isEnglish = !hasJapanese && hasLetter;
}

std::u32string CharacterCount::Summary() const {
    if (isEnglish) {
        return Number(words) + (words == 1 ? U" word (" : U" words (") + Number(total) +
               (total == 1 ? U" char)" : U" chars)");
    }
    if (withoutWhitespace == total) return Number(total) + U"文字";
    return Number(total) + U"文字（空白除く " + Number(withoutWhitespace) + U"）";
}

} // namespace iroha
