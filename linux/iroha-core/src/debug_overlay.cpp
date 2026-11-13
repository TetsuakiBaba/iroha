#include "iroha/debug_overlay.h"

#include <cstdio>
#include <vector>

#include "iroha/unicode.h"

namespace iroha::debug_overlay {

namespace {

std::u32string Number(long long value) { return Utf8ToUtf32(std::to_string(value)); }

// 末尾 20 文字を「」で囲む（窓の幅を抑える。長ければ先頭を … にする）
std::u32string Quoted(const std::u32string& text) {
    constexpr size_t kQuotedLength = 20;
    return text.size() > kQuotedLength ? U"「…" + text.substr(text.size() - kQuotedLength) + U"」"
                                       : U"「" + text + U"」";
}

std::u32string NeuralNetworkPart(const InferenceStats& stats, bool showsCount) {
    if (stats.neuralNetworkCalls == 0) {
        // NN を通さずに返した。理由がわかれば添える（読み全体が学習・ユーザ辞書と一致した、など）
        std::vector<std::u32string> reasons;
        if (stats.shortcuts & InferenceStats::kLearning) reasons.push_back(U"学習");
        if (stats.shortcuts & InferenceStats::kUserDictionary) reasons.push_back(U"ユーザ辞書");
        if (stats.shortcuts & InferenceStats::kCache) reasons.push_back(U"キャッシュ");
        if (reasons.empty()) return U"NN なし";
        std::u32string joined;
        for (const auto& reason : reasons) joined += (joined.empty() ? U"" : U"・") + reason;
        return U"NN なし（" + joined + U"）";
    }
    std::u32string time = U"NN " + FormatDuration(stats.neuralNetworkMicros);
    return showsCount ? time + U"（" + Number(stats.neuralNetworkCalls) + U"回）" : time;
}

std::u32string Join(const std::vector<std::u32string>& parts) {
    std::u32string line;
    for (const auto& part : parts) line += (line.empty() ? U"" : U" ・ ") + part;
    return line;
}

const char32_t* KindName(ConversionKind kind) {
    switch (kind) {
    case ConversionKind::Live: return U"ライブ変換";
    case ConversionKind::Segmenting: return U"文節に分ける";
    case ConversionKind::Resegmenting: return U"文節の区切り直し";
    case ConversionKind::Candidates: return U"候補ウィンドウ";
    }
    return U"";
}

} // namespace

std::u32string FormatDuration(int64_t micros) {
    const double milliseconds = static_cast<double>(micros) / 1000.0;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), milliseconds < 100 ? "%.1fms" : "%.0fms", milliseconds);
    return Utf8ToUtf32(buffer);
}

std::u32string ContextLine(ContextSource source, const std::u32string& documentText,
                           const std::optional<std::u32string>& fallback) {
    std::u32string line = U"左文脈  ";
    switch (source) {
    case ContextSource::Document:
        // 左文脈はカーソルのある行だけなので、行の頭では 0 文字になる
        line += documentText.empty() ? std::u32string(U"アプリから 0文字（カーソルが行の頭）")
                                     : U"アプリから " + Number(static_cast<long long>(documentText.size())) +
                                           U"文字 " + Quoted(documentText);
        break;
    case ContextSource::Disabled:
        line += U"アプリから読む設定が OFF";
        break;
    case ContextSource::Unavailable:
        line += U"アプリから読めない（周辺テキストを返さない）";
        break;
    }
    if (fallback) {
        line += fallback->empty() ? U" → 代わりの確定文字列もなし（文脈なし）"
                                  : U" → 代わりに確定文字列 " + Number(static_cast<long long>(fallback->size())) +
                                        U"文字 " + Quoted(*fallback);
    }
    return line;
}

std::u32string ConversionLine(ConversionKind kind, int64_t totalMicros, const InferenceStats& stats,
                              size_t readingLength, int cancelled) {
    std::vector<std::u32string> parts;
    parts.push_back(std::u32string(KindName(kind)) + U"  全体 " + FormatDuration(totalMicros));
    parts.push_back(NeuralNetworkPart(stats, true));
    parts.push_back(U"読み " + Number(static_cast<long long>(readingLength)) + U"文字");
    if (stats.modelLoadMicros > 0) {
        parts.push_back(U"モデル読み込み " + FormatDuration(stats.modelLoadMicros) + U"（全体に含む）");
    }
    if (cancelled > 0) parts.push_back(U"取り消し " + Number(cancelled));
    return Join(parts);
}

std::u32string TypoLine(TypoTrigger trigger, int64_t totalMicros, const InferenceStats& stats, size_t readingLength,
                        TypoOutcome outcome) {
    std::vector<std::u32string> parts;
    parts.push_back(std::u32string(U"打ち間違い（") + (trigger == TypoTrigger::Idle ? U"入力の休止" : U"スペース") +
                    U"）  全体 " + FormatDuration(totalMicros));
    parts.push_back(NeuralNetworkPart(stats, false));
    parts.push_back(U"読み " + Number(static_cast<long long>(readingLength)) + U"文字");
    if (stats.modelLoadMicros > 0) {
        parts.push_back(U"モデル読み込み " + FormatDuration(stats.modelLoadMicros) + U"（全体に含む）");
    }
    parts.push_back(outcome == TypoOutcome::Corrected ? U"訂正した"
                    : outcome == TypoOutcome::None    ? U"訂正なし"
                                                      : U"末尾に足しただけなので捨てた");
    return Join(parts);
}

} // namespace iroha::debug_overlay
