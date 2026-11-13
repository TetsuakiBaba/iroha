// 移植元は number.h のコメントを参照
#include "lattice/number.h"

#include <array>

#include "iroha/unicode.h"

namespace iroha::lattice {

namespace {

enum class Token {
    One, Two, Three, Four, Five, Six, Seven, Eight, Nine, Zero,
    Ten, Hundred, Thousand, TenThousand, HundredMillion, Trillion,
    End,
    Error,
};

// 0〜9 の数字そのもの（と終わり）
bool IsDigitToken(Token token) {
    return token <= Token::Zero || token == Token::End;
}

// 位（十・百・千・万・億・兆）と終わり・エラー
bool IsUnitToken(Token token) {
    return token >= Token::Ten;
}

const char32_t* ToRoman(Token token) {
    switch (token) {
    case Token::One: return U"1";
    case Token::Two: return U"2";
    case Token::Three: return U"3";
    case Token::Four: return U"4";
    case Token::Five: return U"5";
    case Token::Six: return U"6";
    case Token::Seven: return U"7";
    case Token::Eight: return U"8";
    case Token::Nine: return U"9";
    case Token::Zero: return U"0";
    default: return U"";
    }
}

const char32_t* ToKanji(Token token) {
    switch (token) {
    case Token::One: return U"一";
    case Token::Two: return U"二";
    case Token::Three: return U"三";
    case Token::Four: return U"四";
    case Token::Five: return U"五";
    case Token::Six: return U"六";
    case Token::Seven: return U"七";
    case Token::Eight: return U"八";
    case Token::Nine: return U"九";
    case Token::Zero: return U"〇";
    case Token::Ten: return U"十";
    case Token::Hundred: return U"百";
    case Token::Thousand: return U"千";
    case Token::TenThousand: return U"万";
    case Token::HundredMillion: return U"億";
    case Token::Trillion: return U"兆";
    default: return U"";
    }
}

// 4 桁ごとの区切りとしての大きさ（終わり = 1、万 = 2、億 = 3、兆 = 4）
int MaxDigit(Token token) {
    switch (token) {
    case Token::End: return 1;
    case Token::TenThousand: return 2;
    case Token::HundredMillion: return 3;
    case Token::Trillion: return 4;
    default: return 0;
    }
}

// 読みを数の語に分ける。数の読みでない所で Error を入れて止める。
// 先読みの扱い（「シ」の次が「チ」でないとき、その次の 1 文字を読み飛ばす）は元の実装のまま
std::vector<Token> ParseLiteral(const std::u32string& input) {
    size_t position = 0;
    std::optional<char32_t> lookahead;
    if (position < input.size()) lookahead = input[position++];
    const auto next = [&]() -> std::optional<char32_t> {
        const std::optional<char32_t> current = lookahead;
        lookahead = position < input.size() ? std::optional<char32_t>(input[position++])
                                            : std::nullopt;
        return current;
    };

    std::vector<Token> tokens;
    const auto fail = [&]() {
        tokens.push_back(Token::Error);
        return tokens;
    };
    // 次の 1 文字が expected ならそれを読んで真
    const auto expect = [&](char32_t expected) {
        const std::optional<char32_t> scalar = next();
        return scalar && *scalar == expected;
    };

    while (const std::optional<char32_t> u0 = next()) {
        switch (*u0) {
        case U'イ': {
            const std::optional<char32_t> u1 = next();
            if (u1 && (*u1 == U'チ' || *u1 == U'ッ')) {
                tokens.push_back(Token::One);
            } else {
                return fail();
            }
            break;
        }
        case U'オ':
            if (!expect(U'ク')) return fail();
            tokens.push_back(Token::HundredMillion);
            break;
        case U'キ':
            if (!expect(U'ュ')) return fail();
            if (!expect(U'ウ')) return fail();
            tokens.push_back(Token::Nine);
            break;
        case U'ク':
            tokens.push_back(Token::Nine);
            break;
        case U'ゴ':
            tokens.push_back(Token::Five);
            break;
        case U'サ':
            if (!expect(U'ン')) return fail();
            tokens.push_back(Token::Three);
            break;
        case U'シ': {
            const std::optional<char32_t> u1 = next();
            if (u1 && *u1 == U'チ') {
                tokens.push_back(Token::Seven);
            } else {
                tokens.push_back(Token::Four);
                if (u1) lookahead = u1;
            }
            break;
        }
        case U'ジ': {
            if (!expect(U'ュ')) return fail();
            const std::optional<char32_t> u2 = next();
            if (u2 && (*u2 == U'ウ' || *u2 == U'ッ')) {
                tokens.push_back(Token::Ten);
            } else {
                return fail();
            }
            break;
        }
        case U'セ':
            if (!expect(U'ン')) return fail();
            tokens.push_back(Token::Thousand);
            break;
        case U'ゼ': {
            const std::optional<char32_t> u1 = next();
            if (u1 && *u1 == U'ロ') {
                tokens.push_back(Token::Zero);
            } else if (u1 && *u1 == U'ン') {
                tokens.push_back(Token::Thousand);
            } else {
                return fail();
            }
            break;
        }
        case U'チ':
            if (!expect(U'ョ')) return fail();
            if (!expect(U'ウ')) return fail();
            tokens.push_back(Token::Trillion);
            break;
        case U'ナ':
            if (!expect(U'ナ')) return fail();
            tokens.push_back(Token::Seven);
            break;
        case U'ニ':
            tokens.push_back(Token::Two);
            break;
        case U'ハ': {
            const std::optional<char32_t> u1 = next();
            if (u1 && (*u1 == U'チ' || *u1 == U'ッ')) {
                tokens.push_back(Token::Eight);
            } else {
                return fail();
            }
            break;
        }
        case U'ヒ':
        case U'ビ':
        case U'ピ':
            if (!expect(U'ャ')) return fail();
            if (!expect(U'ク')) return fail();
            tokens.push_back(Token::Hundred);
            break;
        case U'マ': {
            const std::optional<char32_t> u1 = next();
            if (u1 && *u1 == U'ン') {
                tokens.push_back(Token::TenThousand);
            } else if (u1 && *u1 == U'ル') {
                tokens.push_back(Token::Zero);
            } else {
                return fail();
            }
            break;
        }
        case U'ヨ':
            if (!expect(U'ン')) return fail();
            tokens.push_back(Token::Four);
            break;
        case U'レ':
            if (!expect(U'イ')) return fail();
            tokens.push_back(Token::Zero);
            break;
        case U'ロ': {
            const std::optional<char32_t> u1 = next();
            if (u1 && (*u1 == U'ク' || *u1 == U'ッ')) {
                tokens.push_back(Token::Six);
            } else {
                return fail();
            }
            break;
        }
        default:
            return fail();
        }
    }
    tokens.push_back(Token::End);
    return tokens;
}

int DigitValue(Token token) {
    switch (token) {
    case Token::One: return 1;
    case Token::Two: return 2;
    case Token::Three: return 3;
    case Token::Four: return 4;
    case Token::Five: return 5;
    case Token::Six: return 6;
    case Token::Seven: return 7;
    case Token::Eight: return 8;
    case Token::Nine: return 9;
    default: return 0;
    }
}

// 4 桁（千・百・十・一の位）のまとまり
using Group = std::array<int, 4>;

// 語の列を 4 桁ごとのまとまり（大きい位から）にする。数として読めなければ空
std::vector<Group> ParseTokens(const std::vector<Token>& tokens) {
    int maxDigits = 0; // 0 = まだ決まっていない
    std::vector<Group> result;
    Group stack{0, 0, 0, 0};
    std::optional<int> current;
    for (const Token token : tokens) {
        switch (token) {
        case Token::One: case Token::Two: case Token::Three: case Token::Four:
        case Token::Five: case Token::Six: case Token::Seven: case Token::Eight:
        case Token::Nine: case Token::Zero:
            if (current) return {};
            current = DigitValue(token);
            break;
        case Token::Ten:
            stack[2] = current.value_or(1);
            current.reset();
            break;
        case Token::Hundred:
            stack[1] = current.value_or(1);
            current.reset();
            break;
        case Token::Thousand:
            stack[0] = current.value_or(1);
            current.reset();
            break;
        case Token::End: case Token::TenThousand: case Token::HundredMillion:
        case Token::Trillion: {
            stack[3] = current.value_or(0);
            const int digit = MaxDigit(token);
            if (maxDigits != 0) {
                if (maxDigits <= digit) return {};
                result[maxDigits - digit] = stack;
            } else {
                maxDigits = digit;
                result.assign(maxDigits, Group{0, 0, 0, 0});
                result[0] = stack;
            }
            current.reset();
            stack = Group{0, 0, 0, 0};
            break;
        }
        case Token::Error:
            break;
        }
    }
    return result;
}

void AppendDigits(std::u32string* out, const Group& group, int from) {
    for (int i = from; i < 4; ++i) out->push_back(U'0' + group[i]);
}

} // namespace

std::vector<DicdataElement> JapaneseNumberDicdata(const std::u32string& head) {
    const std::vector<Token> tokens = ParseLiteral(head);
    bool allDigits = true;
    bool allUnits = true;
    std::u32string kanji;
    for (const Token token : tokens) {
        if (token == Token::Error) return {};
        allDigits = allDigits && IsDigitToken(token);
        allUnits = allUnits && IsUnitToken(token);
        kanji += ToKanji(token);
    }

    std::u32string roman;
    if (allDigits) {
        for (const Token token : tokens) roman += ToRoman(token);
    } else if (allUnits) {
        return {};
    } else {
        const std::vector<Group> groups = ParseTokens(tokens);
        if (groups.empty()) return {};
        for (const Group& group : groups) {
            if (!roman.empty()) {
                AppendDigits(&roman, group, 0);
            } else if (group[0] != 0) {
                AppendDigits(&roman, group, 0);
            } else if (group[1] != 0) {
                AppendDigits(&roman, group, 1);
            } else if (group[2] != 0) {
                AppendDigits(&roman, group, 2);
            } else if (group[3] != 0) {
                AppendDigits(&roman, group, 3);
            } else {
                return {};
            }
        }
    }
    const PValue kanjiValue = PValue(-17) + static_cast<PValue>(GraphemeCount(head)) / PValue(3);
    const PValue romanValue = PValue(-16) + PValue(4) / static_cast<PValue>(GraphemeCount(roman));
    return {
        DicdataElement(kanji, head, cid::kNumber, mid::kNumber, kanjiValue),
        DicdataElement(roman, head, cid::kNumber, mid::kNumber, romanValue),
    };
}

std::optional<int64_t> ParseInteger(const std::u32string& text) {
    size_t index = 0;
    bool negative = false;
    if (!text.empty() && (text[0] == U'+' || text[0] == U'-')) {
        negative = text[0] == U'-';
        index = 1;
    }
    if (index >= text.size()) return std::nullopt;
    // 負の側で積み上げる（最小値を桁あふれなしで読むため）
    int64_t value = 0;
    for (; index < text.size(); ++index) {
        if (text[index] < U'0' || text[index] > U'9') return std::nullopt;
        const int64_t digit = static_cast<int64_t>(text[index] - U'0');
        if (value < (INT64_MIN + digit) / 10) return std::nullopt;
        value = value * 10 - digit;
    }
    if (negative) return value;
    if (value == INT64_MIN) return std::nullopt;
    return -value;
}

namespace {

const char32_t* const kKanjiDigits[] = {U"〇", U"一", U"二", U"三", U"四",
                                        U"五", U"六", U"七", U"八", U"九"};

// 0 以上の数を漢数字で書く
std::u32string SpellOutNonNegative(uint64_t number) {
    struct Unit {
        uint64_t base;
        const char32_t* name;
        bool omitOne; // 「一十」ではなく「十」と書く位
    };
    static const Unit kUnits[] = {
        {10000000000000000ULL, U"京", false}, {1000000000000ULL, U"兆", false},
        {100000000ULL, U"億", false},         {10000ULL, U"万", false},
        {1000ULL, U"千", true},               {100ULL, U"百", true},
        {10ULL, U"十", true},
    };
    if (number < 10) return kKanjiDigits[number];
    for (const Unit& unit : kUnits) {
        if (number < unit.base) continue;
        const uint64_t quotient = number / unit.base;
        const uint64_t remainder = number % unit.base;
        std::u32string text;
        if (!(unit.omitOne && quotient == 1)) text += SpellOutNonNegative(quotient);
        text += unit.name;
        if (remainder != 0) text += SpellOutNonNegative(remainder);
        return text;
    }
    return {};
}

} // namespace

std::u32string SpellOutJapanese(int64_t number) {
    if (number < 0) {
        return U"マイナス" + SpellOutNonNegative(~static_cast<uint64_t>(number) + 1);
    }
    return SpellOutNonNegative(static_cast<uint64_t>(number));
}

bool IsNumberCharacter(const std::u32string& character) {
    if (character.empty()) return false;
    const char32_t c = character[0];
    if ((c >= U'0' && c <= U'9') || (c >= 0xFF10 && c <= 0xFF19)) return true;
    if (c == 0x00B2 || c == 0x00B3 || c == 0x00B9 || (c >= 0x00BC && c <= 0x00BE)) return true;
    if (c == 0x2070 || (c >= 0x2074 && c <= 0x2079) || (c >= 0x2080 && c <= 0x2089)) return true;
    if ((c >= 0x2150 && c <= 0x2182) || (c >= 0x2185 && c <= 0x2189)) return true;
    if ((c >= 0x2460 && c <= 0x249B) || (c >= 0x24EA && c <= 0x24FF)) return true;
    if (c >= 0x2776 && c <= 0x2793) return true;
    if (c == 0x3007 || (c >= 0x3021 && c <= 0x3029) || (c >= 0x3038 && c <= 0x303A)) return true;
    if ((c >= 0x3192 && c <= 0x3195) || (c >= 0x3220 && c <= 0x3229)) return true;
    if ((c >= 0x3248 && c <= 0x324F) || (c >= 0x3251 && c <= 0x325F)) return true;
    if ((c >= 0x3280 && c <= 0x3289) || (c >= 0x32B1 && c <= 0x32BF)) return true;
    // 数を表す漢字（一二三…、大字）
    static const std::u32string kKanjiNumbers =
        U"一二三四五六七八九十百千万億兆京零壱弐参拾廿卅卌萬佰仟陸漆捌玖";
    return kKanjiNumbers.find(c) != std::u32string::npos;
}

} // namespace iroha::lattice
