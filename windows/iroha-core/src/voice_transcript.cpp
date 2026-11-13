#include "iroha/voice_transcript.h"

#include <algorithm>
#include <map>

#include "iroha/kana.h"
#include "iroha/unicode.h"

namespace iroha {

namespace {

// macOS版 DocumentText.isKanji と同じ範囲（「々」を含む）
bool IsKanji(char32_t c) {
    return (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
           c == 0x3005;
}

bool IsWhitespace(char32_t c) {
    return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == 0x0B || c == 0x0C || c == 0x85 ||
           c == 0xA0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x1680;
}

// Swift の Character.isNumber のうち、認識結果に出るもの（漢数字は呼び出し側で漢字として除く）
bool IsNumber(char32_t c) {
    return (c >= U'0' && c <= U'9') || (c >= 0xFF10 && c <= 0xFF19) || (c >= 0x2460 && c <= 0x249B) ||
           (c >= 0x2160 && c <= 0x217F) || c == 0x00B2 || c == 0x00B3 || c == 0x00B9;
}

bool IsAsciiGrapheme(const std::u32string& grapheme) {
    return !grapheme.empty() && std::all_of(grapheme.begin(), grapheme.end(), [](char32_t c) { return c < 0x80; });
}

std::u32string LastGrapheme(const std::u32string& text) {
    const std::vector<std::u32string> graphemes = SplitGraphemes(text);
    return graphemes.empty() ? U"" : graphemes.back();
}

std::u32string ReplaceAll(std::u32string text, const std::u32string& from, const std::u32string& to) {
    if (from.empty() || from == to) return text;
    size_t position = 0;
    while ((position = text.find(from, position)) != std::u32string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
    return text;
}

// ローマ字入力で読みに入る記号（RomajiComposer の表と同じ文字にそろえる。macOS版 readingSymbols）
const std::map<std::u32string, std::u32string>& ReadingSymbols() {
    static const std::map<std::u32string, std::u32string> symbols = {
        {U"？", U"？"}, {U"?", U"？"}, {U"！", U"！"}, {U"!", U"！"}, {U"「", U"「"},
        {U"」", U"」"}, {U"・", U"・"}, {U"ー", U"ー"}, {U"〜", U"〜"},
    };
    return symbols;
}

bool IsKanaForReading(char32_t c) {
    return (c >= 0x3041 && c <= 0x3096) || (c >= 0x30A1 && c <= 0x30F6) || c == 0x30FC;
}

} // namespace

VoiceTranscript::VoiceTranscript(const std::vector<ReadingToken>& tokens, const std::u32string& comma,
                                 const std::u32string& period) {
    // 固定部分の直後の空白。英字・数字どうしの間なら 1 つ残し（英単語の間）、それ以外は捨てる
    bool pendingSpace = false;

    const auto appendReading = [this, &pendingSpace](const std::u32string& reading, const std::u32string& surface) {
        pendingSpace = false;
        if (!parts.empty() && parts.back().kind == Part::Kind::Reading) {
            parts.back().text += reading;
            parts.back().surface += surface;
        } else {
            parts.push_back(Part::Reading(reading, surface));
        }
    };
    const auto appendFixed = [this, &pendingSpace](const std::u32string& text) {
        if (!parts.empty() && parts.back().kind == Part::Kind::Fixed) {
            const std::vector<std::u32string> first = SplitGraphemes(text);
            const bool keepsSpace = pendingSpace && IsAsciiGrapheme(LastGrapheme(parts.back().text)) &&
                                    !first.empty() && IsAsciiGrapheme(first.front());
            parts.back().text += (keepsSpace ? U" " : U"") + text;
        } else {
            parts.push_back(Part::Fixed(text));
        }
        pendingSpace = false;
    };

    for (const ReadingToken& token : tokens) {
        // 数字の直後の漢字だけの語（年・月・日・人・時間などの助数詞）は、表記のまま数字とつなげる。
        // 数字と切り離して読みを求めると助数詞の読みを取り違える（「10日」の日 → か）
        if (!pendingSpace && !parts.empty() && parts.back().kind == Part::Kind::Fixed) {
            const std::u32string last = LastGrapheme(parts.back().text);
            const bool lastIsNumber = !last.empty() && std::any_of(last.begin(), last.end(), IsNumber) &&
                                      std::none_of(last.begin(), last.end(), IsKanji);
            if (lastIsNumber && !token.surface.empty() &&
                std::all_of(token.surface.begin(), token.surface.end(), IsKanji)) {
                parts.back().text += token.surface;
                continue;
            }
        }
        if (token.reading && !token.reading->empty()) {
            appendReading(ReplaceAll(ReplaceAll(*token.reading, U"、", comma), U"。", period), token.surface);
            continue;
        }
        // 読みが付かなかった語は 1 文字ずつ見る（「？」のように打鍵で読みに入る記号と、空白を分ける）
        std::u32string fixed;
        const auto flushFixed = [&fixed, &appendFixed] {
            if (fixed.empty()) return;
            appendFixed(fixed);
            fixed.clear();
        };
        for (const std::u32string& character : SplitGraphemes(token.surface)) {
            const auto symbol = ReadingSymbols().find(character);
            if (character.size() == 1 && IsWhitespace(character[0])) {
                flushFixed();
                if (!parts.empty() && parts.back().kind == Part::Kind::Fixed) pendingSpace = true;
            } else if (symbol != ReadingSymbols().end()) {
                flushFixed();
                appendReading(symbol->second, character);
            } else {
                fixed += character;
            }
        }
        flushFixed();
    }
}

std::u32string VoiceTranscript::Surface() const {
    std::u32string text;
    for (const Part& part : parts) text += part.kind == Part::Kind::Reading ? part.surface : part.text;
    return text;
}

std::u32string VoiceTranscript::ReadingText() const {
    std::u32string text;
    for (const Part& part : parts) text += part.text;
    return text;
}

ReadingToken VoiceTranscript::TokenFromRecognition(const std::u32string& display, const std::u32string& lexical) {
    const auto isPunctuation = [](char32_t c) { return c == U'、' || c == U'。' || c == U'，' || c == U'．'; };
    if (!display.empty() && std::all_of(display.begin(), display.end(),
                                        [&](char32_t c) { return IsKanaForReading(c) || isPunctuation(c); })) {
        return {display, KatakanaToHiragana(display)};
    }
    // 英字・数字を含む語は、語彙形がかなでも読みにしない（macOS版と同じく表記のまま固定部分にする）
    if (!display.empty() && !lexical.empty() &&
        std::all_of(display.begin(), display.end(),
                    [&](char32_t c) { return IsKanji(c) || IsKanaForReading(c) || isPunctuation(c); }) &&
        std::all_of(lexical.begin(), lexical.end(), IsKanaForReading)) {
        return {display, KatakanaToHiragana(lexical)};
    }
    return {display, std::nullopt};
}

namespace VoiceComparison {

std::vector<Difference> Differences(const std::u32string& recognized, const std::u32string& converted) {
    const std::vector<std::u32string> a = SplitGraphemes(recognized);
    const std::vector<std::u32string> b = SplitGraphemes(converted);
    if (a == b) return {};
    // 最長共通部分列で、消えた文字（a の位置）と足された文字（b の位置）を決める
    const size_t n = a.size();
    const size_t m = b.size();
    std::vector<std::vector<size_t>> lcs(n + 1, std::vector<size_t>(m + 1, 0));
    for (size_t i = n; i-- > 0;) {
        for (size_t j = m; j-- > 0;) {
            lcs[i][j] = a[i] == b[j] ? lcs[i + 1][j + 1] + 1 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
        }
    }
    std::vector<bool> removed(n, true);
    std::vector<bool> inserted(m, true);
    for (size_t i = 0, j = 0; i < n && j < m;) {
        if (a[i] == b[j]) {
            removed[i] = false;
            inserted[j] = false;
            ++i;
            ++j;
        } else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
            ++i;
        } else {
            ++j;
        }
    }

    // 両方を頭からたどる。消えた文字でも足された文字でもないものは、同じ順に対応している
    struct Hunk {
        size_t aStart, aEnd, bStart, bEnd;
    };
    std::vector<Hunk> hunks;
    size_t i = 0;
    size_t j = 0;
    std::optional<std::pair<size_t, size_t>> start;
    size_t commonRun = 0;
    const auto close = [&hunks, &start](size_t endA, size_t endB) {
        if (!start) return;
        hunks.push_back({start->first, endA, start->second, endB});
        start.reset();
    };
    while (i < n || j < m) {
        if (i < n && removed[i]) {
            if (!start) start.emplace(i, j);
            commonRun = 0;
            ++i;
        } else if (j < m && inserted[j]) {
            if (!start) start.emplace(i, j);
            commonRun = 0;
            ++j;
        } else {
            // 同じ文字。2 文字続いたら、そこまでの違いを 1 か所として閉じる
            ++commonRun;
            if (commonRun == 2) close(i - 1, j - 1);
            ++i;
            ++j;
        }
    }
    close(n - commonRun, m - commonRun);

    const auto join = [](const std::vector<std::u32string>& characters, size_t from, size_t to) {
        std::u32string text;
        for (size_t k = from; k < to; ++k) text += characters[k];
        return text;
    };
    std::vector<Difference> differences;
    for (const Hunk& hunk : hunks) {
        // 前後 1 文字（同じ文字）を付ける
        const size_t before = hunk.aStart > 0 ? 1 : 0;
        const size_t after = hunk.aEnd < n ? 1 : 0;
        differences.push_back({join(a, hunk.aStart - before, hunk.aEnd + after),
                               join(b, hunk.bStart - before, hunk.bEnd + after)});
    }
    return differences;
}

std::u32string Summary(const std::u32string& recognized, const std::u32string& converted, size_t limit) {
    const std::vector<Difference> all = Differences(recognized, converted);
    if (all.empty()) return U"音声認識と同じ";
    std::u32string text = U"音声認識 → iroha: ";
    for (size_t k = 0; k < std::min(limit, all.size()); ++k) {
        if (k > 0) text += U"　／　";
        text += all[k].recognized + U" → " + all[k].converted;
    }
    if (all.size() > limit) text += U"　ほか " + Utf8ToUtf32(std::to_string(all.size() - limit)) + U" か所";
    return text;
}

} // namespace VoiceComparison

} // namespace iroha
