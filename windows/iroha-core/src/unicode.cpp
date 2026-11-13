#include "iroha/unicode.h"

namespace iroha {

namespace {
constexpr char32_t kReplacement = U'�';
}

std::wstring Utf32ToUtf16(const std::u32string& s) {
    std::wstring out;
    out.reserve(s.size());
    for (char32_t cp : s) {
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = kReplacement;
        if (cp < 0x10000) {
            out.push_back(static_cast<wchar_t>(cp));
        } else {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        }
    }
    return out;
}

std::u32string Utf16ToUtf32(std::wstring_view s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char32_t unit = s[i];
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
                out.push_back(0x10000 + ((unit - 0xD800) << 10) + (s[i + 1] - 0xDC00));
                ++i;
            } else {
                out.push_back(kReplacement);
            }
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            out.push_back(kReplacement);
        } else {
            out.push_back(unit);
        }
    }
    return out;
}

std::string Utf32ToUtf8(const std::u32string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (char32_t cp : s) {
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = kReplacement;
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::u32string Utf8ToUtf32(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char b0 = static_cast<unsigned char>(s[i]);
        size_t need = 0;
        char32_t cp = 0;
        if (b0 < 0x80) {
            cp = b0;
        } else if ((b0 & 0xE0) == 0xC0) {
            need = 1;
            cp = b0 & 0x1F;
        } else if ((b0 & 0xF0) == 0xE0) {
            need = 2;
            cp = b0 & 0x0F;
        } else if ((b0 & 0xF8) == 0xF0) {
            need = 3;
            cp = b0 & 0x07;
        } else {
            out.push_back(kReplacement);
            ++i;
            continue;
        }
        if (i + need >= s.size()) {
            // 継続バイト不足
            out.push_back(kReplacement);
            break;
        }
        bool valid = true;
        for (size_t k = 1; k <= need; ++k) {
            const unsigned char b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (b & 0x3F);
        }
        if (!valid || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out.push_back(kReplacement);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += need + 1;
    }
    return out;
}

namespace {
bool IsTrimmableWhitespace(char32_t c) {
    switch (c) {
        case U' ':
        case U'\t':
        case U'\n':
        case U'\r':
        case 0x0B:
        case 0x0C:
        case 0x85:   // NEL
        case 0xA0:   // NBSP
        case 0x1680:
        case 0x2028:
        case 0x2029:
        case 0x202F:
        case 0x205F:
        case 0x3000: // 全角空白
            return true;
        default:
            return c >= 0x2000 && c <= 0x200A;
    }
}
} // namespace

std::u32string TrimWhitespace(const std::u32string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && IsTrimmableWhitespace(s[begin])) ++begin;
    while (end > begin && IsTrimmableWhitespace(s[end - 1])) --end;
    return s.substr(begin, end - begin);
}

namespace {

// 直前の文字に付く文字（結合文字・異体字セレクタ・肌の色の修飾・タグ）
bool IsGraphemeExtend(char32_t c) {
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) ||
           (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF) ||
           c == 0x200C || (c >= 0x302A && c <= 0x302F) ||
           (c >= 0x3099 && c <= 0x309A) || (c >= 0xFE00 && c <= 0xFE0F) ||
           (c >= 0xFE20 && c <= 0xFE2F) || (c >= 0xFF9E && c <= 0xFF9F) ||
           (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0020 && c <= 0xE007F) ||
           (c >= 0xE0100 && c <= 0xE01EF);
}

bool IsRegionalIndicator(char32_t c) { return c >= 0x1F1E6 && c <= 0x1F1FF; }

// 絵文字として描かれる文字（ZWJで次の絵文字とつながる側）のおおよその範囲
bool IsExtendedPictographic(char32_t c) {
    if (IsRegionalIndicator(c) || (c >= 0x1F3FB && c <= 0x1F3FF)) return false;
    return c == 0x00A9 || c == 0x00AE || c == 0x203C || c == 0x2049 || c == 0x2122 ||
           c == 0x2139 || (c >= 0x2194 && c <= 0x21AA) || (c >= 0x231A && c <= 0x23FF) ||
           c == 0x24C2 || (c >= 0x25AA && c <= 0x25FE) || (c >= 0x2600 && c <= 0x27BF) ||
           (c >= 0x2934 && c <= 0x2935) || (c >= 0x2B05 && c <= 0x2B55) || c == 0x3030 ||
           c == 0x303D || c == 0x3297 || c == 0x3299 || (c >= 0x1F000 && c <= 0x1FAFF);
}

constexpr char32_t kZeroWidthJoiner = 0x200D;

// s[begin] から始まる書記素クラスタの終わり（次のクラスタの先頭）
size_t GraphemeEnd(const std::u32string& s, size_t begin) {
    size_t end = begin + 1;
    if (s[begin] == U'\r' && end < s.size() && s[end] == U'\n') return end + 1;
    if (IsRegionalIndicator(s[begin]) && end < s.size() && IsRegionalIndicator(s[end])) ++end;
    const bool pictographic = IsExtendedPictographic(s[begin]);
    while (end < s.size()) {
        if (IsGraphemeExtend(s[end])) {
            ++end;
        } else if (s[end] == kZeroWidthJoiner) {
            ++end;
            if (pictographic && end < s.size() && IsExtendedPictographic(s[end])) ++end;
        } else {
            break;
        }
    }
    return end;
}

} // namespace

std::vector<std::u32string> SplitGraphemes(const std::u32string& s) {
    std::vector<std::u32string> clusters;
    clusters.reserve(s.size());
    for (size_t begin = 0; begin < s.size();) {
        const size_t end = GraphemeEnd(s, begin);
        clusters.push_back(s.substr(begin, end - begin));
        begin = end;
    }
    return clusters;
}

size_t GraphemeCount(const std::u32string& s) {
    size_t count = 0;
    for (size_t begin = 0; begin < s.size(); begin = GraphemeEnd(s, begin)) ++count;
    return count;
}

std::optional<std::u32string> Utf8ToUtf32Strict(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char b0 = static_cast<unsigned char>(s[i]);
        size_t need = 0;
        char32_t cp = 0;
        if (b0 < 0x80) {
            cp = b0;
        } else if ((b0 & 0xE0) == 0xC0) {
            need = 1;
            cp = b0 & 0x1F;
        } else if ((b0 & 0xF0) == 0xE0) {
            need = 2;
            cp = b0 & 0x0F;
        } else if ((b0 & 0xF8) == 0xF0) {
            need = 3;
            cp = b0 & 0x07;
        } else {
            return std::nullopt;
        }
        if (i + need >= s.size()) return std::nullopt;
        for (size_t k = 1; k <= need; ++k) {
            const unsigned char b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xC0) != 0x80) return std::nullopt;
            cp = (cp << 6) | (b & 0x3F);
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return std::nullopt;
        out.push_back(cp);
        i += need + 1;
    }
    return out;
}

} // namespace iroha
