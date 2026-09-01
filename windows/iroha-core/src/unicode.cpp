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
