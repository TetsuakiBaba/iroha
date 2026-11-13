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

namespace {

// 全角カタカナ・記号 → 半角（Unicode の半角形の対応。濁点・半濁点つきは 2 文字になる）
const char32_t* HalfwidthKana(char32_t c) {
    struct Pair {
        char32_t full;
        const char32_t* half;
    };
    static const Pair kPairs[] = {
        {U'ァ', U"ｧ"}, {U'ア', U"ｱ"}, {U'ィ', U"ｨ"}, {U'イ', U"ｲ"}, {U'ゥ', U"ｩ"}, {U'ウ', U"ｳ"},
        {U'ェ', U"ｪ"}, {U'エ', U"ｴ"}, {U'ォ', U"ｫ"}, {U'オ', U"ｵ"}, {U'カ', U"ｶ"}, {U'ガ', U"ｶﾞ"},
        {U'キ', U"ｷ"}, {U'ギ', U"ｷﾞ"}, {U'ク', U"ｸ"}, {U'グ', U"ｸﾞ"}, {U'ケ', U"ｹ"}, {U'ゲ', U"ｹﾞ"},
        {U'コ', U"ｺ"}, {U'ゴ', U"ｺﾞ"}, {U'サ', U"ｻ"}, {U'ザ', U"ｻﾞ"}, {U'シ', U"ｼ"}, {U'ジ', U"ｼﾞ"},
        {U'ス', U"ｽ"}, {U'ズ', U"ｽﾞ"}, {U'セ', U"ｾ"}, {U'ゼ', U"ｾﾞ"}, {U'ソ', U"ｿ"}, {U'ゾ', U"ｿﾞ"},
        {U'タ', U"ﾀ"}, {U'ダ', U"ﾀﾞ"}, {U'チ', U"ﾁ"}, {U'ヂ', U"ﾁﾞ"}, {U'ッ', U"ｯ"}, {U'ツ', U"ﾂ"},
        {U'ヅ', U"ﾂﾞ"}, {U'テ', U"ﾃ"}, {U'デ', U"ﾃﾞ"}, {U'ト', U"ﾄ"}, {U'ド', U"ﾄﾞ"}, {U'ナ', U"ﾅ"},
        {U'ニ', U"ﾆ"}, {U'ヌ', U"ﾇ"}, {U'ネ', U"ﾈ"}, {U'ノ', U"ﾉ"}, {U'ハ', U"ﾊ"}, {U'バ', U"ﾊﾞ"},
        {U'パ', U"ﾊﾟ"}, {U'ヒ', U"ﾋ"}, {U'ビ', U"ﾋﾞ"}, {U'ピ', U"ﾋﾟ"}, {U'フ', U"ﾌ"}, {U'ブ', U"ﾌﾞ"},
        {U'プ', U"ﾌﾟ"}, {U'ヘ', U"ﾍ"}, {U'ベ', U"ﾍﾞ"}, {U'ペ', U"ﾍﾟ"}, {U'ホ', U"ﾎ"}, {U'ボ', U"ﾎﾞ"},
        {U'ポ', U"ﾎﾟ"}, {U'マ', U"ﾏ"}, {U'ミ', U"ﾐ"}, {U'ム', U"ﾑ"}, {U'メ', U"ﾒ"}, {U'モ', U"ﾓ"},
        {U'ャ', U"ｬ"}, {U'ヤ', U"ﾔ"}, {U'ュ', U"ｭ"}, {U'ユ', U"ﾕ"}, {U'ョ', U"ｮ"}, {U'ヨ', U"ﾖ"},
        {U'ラ', U"ﾗ"}, {U'リ', U"ﾘ"}, {U'ル', U"ﾙ"}, {U'レ', U"ﾚ"}, {U'ロ', U"ﾛ"}, {U'ワ', U"ﾜ"},
        {U'ヲ', U"ｦ"}, {U'ン', U"ﾝ"}, {U'ヴ', U"ｳﾞ"}, {U'ヷ', U"ﾜﾞ"}, {U'ヺ', U"ｦﾞ"}, {U'・', U"･"},
        {U'ー', U"ｰ"}, {U'、', U"､"}, {U'。', U"｡"}, {U'「', U"｢"}, {U'」', U"｣"}, {U'゛', U"ﾞ"},
        {U'゜', U"ﾟ"},
    };
    for (const Pair& pair : kPairs) {
        if (pair.full == c) return pair.half;
    }
    return nullptr;
}

} // namespace

std::u32string ToHalfwidth(const std::u32string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (char32_t cp : s) {
        if (cp >= 0xFF01 && cp <= 0xFF5E) {
            out.push_back(cp - 0xFF01 + 0x21);
        } else if (cp == 0x3000) {
            out.push_back(U' ');
        } else if (const char32_t* half = HalfwidthKana(cp)) {
            out += half;
        } else {
            out.push_back(cp);
        }
    }
    return out;
}

std::u32string ToFullwidth(const std::u32string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (char32_t cp : s) {
        if (cp >= 0x21 && cp <= 0x7E) {
            out.push_back(cp - 0x21 + 0xFF01);
        } else if (cp == U' ') {
            out.push_back(0x3000);
        } else {
            out.push_back(cp);
        }
    }
    return out;
}

} // namespace iroha
