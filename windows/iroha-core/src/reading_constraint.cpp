#include "iroha/reading_constraint.h"

#include "iroha/kana.h"

namespace iroha {

std::optional<ReadingConstraint> ReadingConstraint::Create(
    const std::u32string& reading) {
    const std::u32string characters = KatakanaToHiragana(reading);
    if (characters.empty() ||
        characters.size() > static_cast<size_t>(kMaxReadingLength)) {
        return std::nullopt;
    }
    std::unordered_map<char32_t, uint64_t> positions;
    for (size_t index = 0; index < characters.size(); ++index) {
        positions[characters[index]] |= uint64_t{1} << index;
    }
    return ReadingConstraint(static_cast<int>(characters.size()),
                             std::move(positions));
}

ReadingConstraint::ReadingConstraint(
    int readingLength, std::unordered_map<char32_t, uint64_t> literalPositions)
    : readingLength_(readingLength),
      literalPositions_(std::move(literalPositions)) {}

bool ReadingConstraint::IsComplete(uint64_t mask) const {
    return (mask & (uint64_t{1} << readingLength_)) != 0;
}

uint64_t ReadingConstraint::Advance(uint64_t mask, const std::u32string& text) const {
    uint64_t current = mask;
    for (char32_t character : text) {
        current = Advance(current, character);
        if (current == 0) return 0;
    }
    return current;
}

uint64_t ReadingConstraint::Advance(uint64_t mask, char32_t character) const {
    char32_t normalized = character;
    if (character >= 0x30A1 && character <= 0x30F6) normalized = character - 0x60;
    // 読みの同じ文字に重なる位置は1文字進める
    auto it = literalPositions_.find(normalized);
    uint64_t next = (mask & (it != literalPositions_.end() ? it->second : 0)) << 1;
    const Span span = SpanOf(character);
    if (span != Span::Literal) {
        // 読みが不定の文字は1〜maxSpan文字を消費したとみなす
        uint64_t spread = mask;
        for (int i = 0; i < kMaxSpan; ++i) {
            spread <<= 1;
            next |= spread;
        }
        // 英数字とカタカナは読みを消費しないこともある（WOWOW←わうわう、コンピューター←こんぴゅーた）
        if (span == Span::ZeroOrMore) next |= mask;
    }
    // 読みの長さを超えた位置は捨てる
    const uint64_t overflow = ~uint64_t{0} << (readingLength_ + 1);
    return next & ~overflow;
}

ReadingConstraint::Span ReadingConstraint::SpanOf(char32_t c) {
    // ひらがな（濁点・繰り返し記号を含む）
    if (c >= 0x3041 && c <= 0x309F) return Span::Literal;
    // 々（読みは直前の漢字次第）
    if (c == 0x3005) return Span::OneOrMore;
    // 、。「」・… などの和文記号
    if (c >= 0x3000 && c <= 0x303F) return Span::Literal;
    // ASCII記号・空白
    if ((c >= 0x20 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) ||
        (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E)) {
        return Span::Literal;
    }
    // 全角記号
    if ((c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) ||
        (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65)) {
        return Span::Literal;
    }
    // カタカナ（長音符を含む）・半角カナ
    if ((c >= 0x30A0 && c <= 0x30FF) || (c >= 0xFF66 && c <= 0xFF9F)) {
        return Span::ZeroOrMore;
    }
    // ASCII英数字
    if ((c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5A) ||
        (c >= 0x61 && c <= 0x7A)) {
        return Span::ZeroOrMore;
    }
    // 全角英数字
    if ((c >= 0xFF10 && c <= 0xFF19) || (c >= 0xFF21 && c <= 0xFF3A) ||
        (c >= 0xFF41 && c <= 0xFF5A)) {
        return Span::ZeroOrMore;
    }
    // 漢字など
    return Span::OneOrMore;
}

} // namespace iroha
