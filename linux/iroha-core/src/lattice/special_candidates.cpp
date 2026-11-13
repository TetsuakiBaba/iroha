// 移植元は special_candidates.h のコメントを参照
#include "lattice/special_candidates.h"

#include <cstdint>
#include <optional>

#include "iroha/unicode.h"
#include "lattice/dicdata_store.h"
#include "lattice/number.h"

namespace iroha::lattice {

namespace {

bool IsAsciiDigit(const std::u32string& character) {
    return character.size() == 1 && character[0] >= U'0' && character[0] <= U'9';
}

std::u32string Join(const std::vector<std::u32string>& characters, size_t begin, size_t end) {
    std::u32string text;
    for (size_t i = begin; i < end && i < characters.size(); ++i) text += characters[i];
    return text;
}

std::u32string ToString(int64_t number) {
    const std::string digits = std::to_string(number);
    return std::u32string(digits.begin(), digits.end());
}

std::u32string TwoDigits(int64_t number) {
    std::u32string text = ToString(number);
    if (text.size() < 2) text.insert(text.begin(), U'0');
    return text;
}

bool HasPrefix(const std::vector<std::u32string>& characters, const std::u32string& prefix) {
    const std::vector<std::u32string> expected = SplitGraphemes(prefix);
    if (characters.size() < expected.size()) return false;
    return std::equal(expected.begin(), expected.end(), characters.begin());
}

bool HasSuffix(const std::vector<std::u32string>& characters, const std::u32string& suffix) {
    const std::vector<std::u32string> expected = SplitGraphemes(suffix);
    if (characters.size() < expected.size()) return false;
    return std::equal(expected.begin(), expected.end(), characters.end() - expected.size());
}

// 桁区切りの数字（「12345」→ 12,345、「-1234.5」→ -1,234.5）
void AppendCommaSeparatedNumber(const std::u32string& convertTarget,
                                std::vector<DicdataElement>* result) {
    std::vector<std::u32string> text = SplitGraphemes(convertTarget);
    if (text.empty()) return;
    bool negative = false;
    if (text.front() == U"-") {
        negative = true;
        text.erase(text.begin());
    }
    // 「.」で分けた部分は 2 つまでで、どれも空でなく ASCII の数字だけ
    std::vector<std::vector<std::u32string>> parts(1);
    for (const std::u32string& character : text) {
        if (character == U".") {
            parts.emplace_back();
        } else {
            parts.back().push_back(character);
        }
    }
    if (parts.size() > 2) return;
    for (const auto& part : parts) {
        if (part.empty()) return;
        for (const std::u32string& character : part) {
            if (!IsAsciiDigit(character)) return;
        }
    }
    const std::vector<std::u32string>& integerPart = parts[0];
    if (integerPart.size() <= 3) return;

    std::u32string formatted = negative ? U"-" : U"";
    for (size_t i = 0; i < integerPart.size(); ++i) {
        if (i > 0 && (integerPart.size() - i) % 3 == 0) formatted += U",";
        formatted += integerPart[i];
    }
    if (parts.size() == 2) formatted += U"." + Join(parts[1], 0, parts[1].size());
    result->emplace_back(formatted, ToKatakana(convertTarget), cid::kProperNoun, mid::kGeneral,
                         PValue(-10));
}

// 時刻（「930」→ 9:30、「1230」→ 12:30）
void AppendTimeExpression(const std::u32string& convertTarget,
                          std::vector<DicdataElement>* result) {
    const std::vector<std::u32string> digits = SplitGraphemes(convertTarget);
    for (const std::u32string& character : digits) {
        if (!IsAsciiDigit(character)) return;
    }
    std::u32string expression;
    if (digits.size() == 3) {
        const int64_t first = *ParseInteger(Join(digits, 0, 1));
        const int64_t last = *ParseInteger(Join(digits, 1, 3));
        if (last > 59) return;
        expression = ToString(first) + U":" + TwoDigits(last);
    } else if (digits.size() == 4) {
        const int64_t first = *ParseInteger(Join(digits, 0, 2));
        const int64_t last = *ParseInteger(Join(digits, 2, 4));
        if (first > 24 || last > 59) return;
        expression = TwoDigits(first) + U":" + TwoDigits(last);
    } else {
        return;
    }
    result->emplace_back(expression, convertTarget, cid::kProperNoun, mid::kGeneral, PValue(-10));
}

// 西暦の年（「2026ネン」）から和暦
void AppendWareki(const std::u32string& katakana, const std::vector<std::u32string>& characters,
                  std::vector<DicdataElement>* result) {
    const auto add = [&](const std::u32string& text, PValue value) {
        result->emplace_back(text, katakana, cid::kCommonNoun, mid::kYear, value);
    };
    const std::optional<int64_t> seireki = ParseInteger(Join(characters, 0, 4));
    if (!seireki) return;
    if (!HasSuffix(characters, U"ネン")) return;
    const int64_t year = *seireki;
    if (year == 1989) {
        add(U"平成元年", -18);
        add(U"昭和64年", -19);
    } else if (year == 2019) {
        add(U"令和元年", -18);
        add(U"平成31年", -19);
    } else if (year == 1926) {
        add(U"昭和元年", -18);
        add(U"大正15年", -19);
    } else if (year == 1912) {
        add(U"大正元年", -18);
        add(U"明治45年", -19);
    } else if (year == 1868) {
        add(U"明治元年", -18);
        add(U"慶應4年", -19);
    } else if (year >= 1990 && year <= 2018) {
        add(U"平成" + ToString(year - 1988) + U"年", -18);
    } else if (year >= 1927 && year <= 1988) {
        add(U"昭和" + ToString(year - 1925) + U"年", -18);
    } else if (year >= 1869 && year <= 1911) {
        // 元の実装は 1967 を引いている（「1900ねん」→「明治-67年」。1867 の書き間違いとみられる）。
        // macOS 版も iroha の EraYearCandidates.swift で 1867 に直してある（2026-11-13）
        add(U"明治" + ToString(year - 1867) + U"年", -18);
    } else if (year >= 1912 && year <= 1926) {
        add(U"大正" + ToString(year - 1911) + U"年", -18);
    } else if (year >= 2020) {
        add(U"令和" + ToString(year - 2018) + U"年", -18);
    }
}

// 和暦の年（「レイワ8ネン」）から西暦
std::optional<std::u32string> ToSeireki(const std::u32string& katakana,
                                        const std::vector<std::u32string>& characters) {
    if (katakana == U"メイジガンネン") return U"1868年";
    if (katakana == U"タイショウガンネン") return U"1912年";
    if (katakana == U"ショウワガンネン") return U"1926年";
    if (katakana == U"ヘイセイガンネン") return U"1989年";
    if (katakana == U"レイワガンネン") return U"2019年";
    if (!HasSuffix(characters, U"ネン")) return std::nullopt;

    struct Era {
        const char32_t* name;
        size_t nameLength;
        int64_t offset;
    };
    static const Era kEras[] = {
        {U"ショウワ", 4, 1925}, {U"ヘイセイ", 4, 1988}, {U"レイワ", 3, 2018},
        {U"メイジ", 3, 1867},   {U"タイショウ", 5, 1911},
    };
    const size_t count = characters.size();
    for (const Era& era : kEras) {
        if (!HasPrefix(characters, era.name)) continue;
        // 年の数字は 1 桁か 2 桁（全体の文字数 = 元号 + 数字 + 「ネン」）
        if (count != era.nameLength + 3 && count != era.nameLength + 4) return std::nullopt;
        const std::optional<int64_t> year = ParseInteger(Join(characters, era.nameLength, count - 2));
        if (!year) return std::nullopt;
        return ToString(*year + era.offset) + U"年";
    }
    return std::nullopt;
}

} // namespace

std::vector<DicdataElement> SpecialCandidates(const std::u32string& convertTarget) {
    std::vector<DicdataElement> result;
    AppendCommaSeparatedNumber(convertTarget, &result);
    AppendTimeExpression(convertTarget, &result);
    const std::u32string katakana = ToKatakana(convertTarget);
    const std::vector<std::u32string> characters = SplitGraphemes(katakana);
    AppendWareki(katakana, characters, &result);
    if (const std::optional<std::u32string> seireki = ToSeireki(katakana, characters)) {
        result.emplace_back(*seireki, katakana, cid::kProperNoun, mid::kGeneral, PValue(-15));
    }
    return result;
}

} // namespace iroha::lattice
