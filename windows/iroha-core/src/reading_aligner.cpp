#include "iroha/reading_aligner.h"

#include <algorithm>

#include "iroha/kana.h"

namespace iroha {
namespace ReadingAligner {

namespace {

struct Run {
    std::u32string text;
    bool isKana;
};

bool IsHiraganaLike(char32_t c) {
    // ひらがな + 長音符・繰り返し記号・句読点類は「読みにそのまま現れる」扱い
    if (c >= 0x3041 && c <= 0x309F) return true;
    static const std::u32string extras = U"ー、。！？，．・「」";
    return extras.find(c) != std::u32string::npos;
}

bool AllHiraganaLike(const std::u32string& text) {
    return std::all_of(text.begin(), text.end(), IsHiraganaLike);
}

std::vector<Run> SplitRuns(const std::u32string& text) {
    std::vector<Run> runs;
    for (char32_t character : text) {
        // 長音符は直前のラン（カタカナ語の途中など）に追随させる
        if (character == U'ー' && !runs.empty()) {
            runs.back().text.push_back(character);
            continue;
        }
        const bool isKana = IsHiraganaLike(character);
        if (!runs.empty() && runs.back().isKana == isKana) {
            runs.back().text.push_back(character);
        } else {
            runs.push_back(Run{std::u32string(1, character), isKana});
        }
    }
    return runs;
}

// Swift版の正規表現 ^リテラル(.+?)リテラル…$ と同じ「最左最短」の割り当てを、
// 限定パターン専用のバックトラックで再現する。
// piece.literalが空のものは最短マッチグループ（1文字以上）を表す。
struct PatternPiece {
    bool isGroup;
    std::u32string literal;
};

bool MatchLazy(const std::u32string& reading, const std::vector<PatternPiece>& pieces,
               size_t pos, size_t pieceIndex,
               std::vector<std::pair<size_t, size_t>>* groups) {
    if (pieceIndex == pieces.size()) return pos == reading.size();
    const PatternPiece& piece = pieces[pieceIndex];
    if (!piece.isGroup) {
        if (reading.compare(pos, piece.literal.size(), piece.literal) != 0) return false;
        return MatchLazy(reading, pieces, pos + piece.literal.size(), pieceIndex + 1,
                         groups);
    }
    // 遅延量指定子(.+?): 短い割り当てから試す
    for (size_t length = 1; pos + length <= reading.size(); ++length) {
        groups->push_back({pos, length});
        if (MatchLazy(reading, pieces, pos + length, pieceIndex + 1, groups)) {
            return true;
        }
        groups->pop_back();
    }
    return false;
}

} // namespace

std::vector<Segment> SegmentReading(const std::u32string& reading,
                                    const std::u32string& conversion) {
    const std::vector<Segment> whole = {Segment{reading, conversion}};
    if (reading.empty() || conversion.empty()) return whole;

    // 変換結果を「ひらがな連続」と「それ以外（漢字・カタカナ・英数）」のランに分ける
    const std::vector<Run> runs = SplitRuns(conversion);
    // 全部ひらがな（=無変換）なら分割しない
    if (std::all_of(runs.begin(), runs.end(), [](const Run& r) { return r.isKana; })) {
        return whole;
    }

    // ひらがなランをリテラル、それ以外を最短マッチのグループにして読みを照合する
    std::vector<PatternPiece> pattern;
    for (const Run& run : runs) {
        if (run.isKana) {
            pattern.push_back({false, run.text});
        } else {
            // カタカナは読みではひらがなで現れるため、判明している場合はリテラルにする
            const std::u32string hiragana = KatakanaToHiragana(run.text);
            if (hiragana != run.text && AllHiraganaLike(hiragana)) {
                pattern.push_back({false, hiragana});
            } else {
                pattern.push_back({true, {}});
            }
        }
    }

    std::vector<std::pair<size_t, size_t>> groups;
    if (!MatchLazy(reading, pattern, 0, 0, &groups)) return whole;

    // マッチ結果から各ランの読み区間を割り当てる
    struct Piece {
        std::u32string readingPart;
        std::u32string conversionPart;
        bool isKana;
    };
    size_t groupIndex = 0;
    std::vector<Piece> pieces;
    for (const Run& run : runs) {
        if (run.isKana) {
            pieces.push_back({run.text, run.text, true});
        } else {
            const std::u32string hiragana = KatakanaToHiragana(run.text);
            if (hiragana != run.text && AllHiraganaLike(hiragana)) {
                pieces.push_back({hiragana, run.text, false});
            } else {
                if (groupIndex >= groups.size()) return whole;
                const auto [pos, length] = groups[groupIndex];
                pieces.push_back({reading.substr(pos, length), run.text, false});
                ++groupIndex;
            }
        }
    }

    // 文節を構成: 「非かなラン + 直後のかなラン」を1文節にまとめる。
    // 先頭のかなランは独立した文節にする
    std::vector<Segment> segments;
    size_t index = 0;
    while (index < pieces.size()) {
        const Piece& piece = pieces[index];
        if (piece.isKana) {
            segments.push_back(Segment{piece.readingPart, piece.conversionPart});
            ++index;
        } else {
            std::u32string readingPart = piece.readingPart;
            std::u32string conversionPart = piece.conversionPart;
            if (index + 1 < pieces.size() && pieces[index + 1].isKana) {
                readingPart += pieces[index + 1].readingPart;
                conversionPart += pieces[index + 1].conversionPart;
                index += 2;
            } else {
                ++index;
            }
            segments.push_back(Segment{readingPart, conversionPart});
        }
    }
    return segments.empty() ? whole : segments;
}

} // namespace ReadingAligner
} // namespace iroha
