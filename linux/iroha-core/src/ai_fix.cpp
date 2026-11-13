#include "iroha/ai_fix.h"

#include <algorithm>
#include <limits>

#include "iroha/kana.h"
#include "iroha/unicode.h"

namespace iroha {
namespace AIFixRule {

const char32_t kInstructions[] =
    U"あなたは日本語のかな漢字変換の結果を確かめる係です。入力として、前の文脈・読み（カタカナ）・変換結果が与えられます。\n"
    U"変換結果に読みと合わない語や、文脈に合わない同音異義語があれば直してください。正しければそのまま返してください。\n"
    U"読みにない語を足さないこと。言い換えないこと。句読点や記号を足さないこと。前の文脈は出力に含めないこと。\n"
    U"出力は直した変換結果の1行だけにしてください。説明は書かないでください。";

float MinimumMargin(const std::vector<float>& margins) {
    float minimum = std::numeric_limits<float>::infinity();
    for (float margin : margins) minimum = std::min(minimum, margin);
    return minimum;
}

bool IsSuspicious(const std::u32string& scoredText, const std::vector<float>& margins, const std::u32string& shown,
                  const std::u32string& reading, double threshold) {
    if (reading.empty() || GraphemeCount(reading) > kMaximumReadingLength || scoredText != shown || margins.empty()) {
        return false;
    }
    return MinimumMargin(margins) < threshold;
}

std::u32string Message(const std::u32string& context, const std::u32string& reading, const std::u32string& conversion) {
    const std::u32string shownContext = context.empty() ? U"（なし）" : context;
    return U"前の文脈: " + shownContext + U"\n読み: " + HiraganaToKatakana(reading) + U"\n変換結果: " + conversion;
}

std::optional<TypoCorrectionFeedback> Feedback(const std::u32string& original, const std::u32string& corrected,
                                               size_t context, size_t maxChanged) {
    if (original == corrected) return std::nullopt;
    const std::vector<std::u32string> lhs = SplitGraphemes(original);
    const std::vector<std::u32string> rhs = SplitGraphemes(corrected);
    size_t head = 0;
    while (head < lhs.size() && head < rhs.size() && lhs[head] == rhs[head]) ++head;
    size_t tail = 0;
    while (tail < lhs.size() - head && tail < rhs.size() - head &&
           lhs[lhs.size() - 1 - tail] == rhs[rhs.size() - 1 - tail]) {
        ++tail;
    }
    const auto join = [](const std::vector<std::u32string>& parts, size_t from, size_t to) {
        std::u32string text;
        for (size_t i = from; i < to; ++i) text += parts[i];
        return text;
    };
    std::u32string prefix = head > context ? U"…" + join(lhs, head - context, head) : join(lhs, 0, head);
    const size_t suffixStart = lhs.size() - tail;
    std::u32string suffix =
        tail > context ? join(lhs, suffixStart, suffixStart + context) + U"…" : join(lhs, suffixStart, lhs.size());
    const auto abbreviate = [&](const std::vector<std::u32string>& parts, size_t from, size_t to) {
        return to - from > maxChanged ? join(parts, from, from + maxChanged) + U"…" : join(parts, from, to);
    };
    return TypoCorrectionFeedback{prefix, abbreviate(lhs, head, lhs.size() - tail), abbreviate(rhs, head, rhs.size() - tail),
                                  suffix};
}

std::optional<std::u32string> Answer(const std::u32string& output, const std::u32string& current) {
    std::u32string last;
    size_t start = 0;
    while (start <= output.size()) {
        size_t end = output.find_first_of(U"\r\n", start);
        if (end == std::u32string::npos) end = output.size();
        std::u32string line = output.substr(start, end - start);
        // 前後の空白（全角を含む）を除く
        line = TrimWhitespace(line);
        if (!line.empty()) last = line;
        start = end + 1;
    }
    if (last.empty()) return std::nullopt;
    const size_t currentCount = GraphemeCount(current);
    if (GraphemeCount(last) > std::max(currentCount * 2, currentCount + 4)) return std::nullopt;
    return last;
}

} // namespace AIFixRule
} // namespace iroha
