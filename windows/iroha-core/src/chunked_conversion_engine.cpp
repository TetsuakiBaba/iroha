#include "iroha/chunked_conversion_engine.h"
#include "iroha/inference_timer.h"

#include <algorithm>

#include "iroha/reading_aligner.h"

namespace iroha {

ChunkedConversionEngine::ChunkedConversionEngine(ConversionEngine* base, size_t maxChunkLength)
    : base_(base),
      maxChunkLength_(std::max<size_t>(1, maxChunkLength)),
      minChunkLength_(std::max<size_t>(1, maxChunkLength / 3)) {}

bool ChunkedConversionEngine::Convert(const std::u32string& reading,
                                      const std::u32string& context, int candidateCount,
                                      std::vector<std::u32string>* results, std::string* error) {
    if (!results) return false;
    if (reading.size() <= maxChunkLength_) {
        return base_->Convert(reading, context, candidateCount, results, error);
    }

    // 長い読みは候補を並べる場面（文節の候補ウィンドウ）には来ないので、1候補だけ返す
    results->clear();
    std::u32string result;
    std::u32string currentContext = context;
    size_t index = 0;
    while (index < reading.size()) {
        const size_t remaining = reading.size() - index;
        std::u32string converted;
        if (remaining <= maxChunkLength_) {
            if (!ConvertCached(reading.substr(index), currentContext, &converted, error)) {
                return false;
            }
            result += converted;
            break;
        }
        size_t cut = 0;
        if (!CutWindow(reading.substr(index, maxChunkLength_), currentContext, &cut, &converted,
                       error)) {
            return false;
        }
        result += converted;
        currentContext += converted;
        index += cut;
    }
    results->push_back(std::move(result));
    return true;
}

bool ChunkedConversionEngine::CutWindow(const std::u32string& window,
                                        const std::u32string& context, size_t* cut,
                                        std::u32string* converted, std::string* error) {
    // 1. 句読点の直後で切る（次の区切りを文の頭から始められる）
    if (const std::optional<size_t> punctuation = PunctuationCut(window, minChunkLength_)) {
        *cut = *punctuation;
        return ConvertCached(window.substr(0, *cut), context, converted, error);
    }

    // 2. 窓を変換して文節境界で切る。最後の文節は語の途中で切れているかもしれないので次へ回す
    std::u32string whole;
    if (!ConvertCached(window, context, &whole, error)) return false;
    const auto segments = ReadingAligner::SegmentReading(window, whole);
    if (segments.size() >= 2) {
        size_t headLength = 0;
        std::u32string head;
        for (size_t i = 0; i + 1 < segments.size(); ++i) {
            headLength += segments[i].reading.size();
            head += segments[i].conversion;
        }
        if (headLength >= minChunkLength_) {
            *cut = headLength;
            *converted = std::move(head);
            return true;
        }
    }

    // 3. 境界が取れなければ窓ごと使う
    *cut = window.size();
    *converted = std::move(whole);
    return true;
}

std::optional<size_t> ChunkedConversionEngine::PunctuationCut(const std::u32string& window,
                                                              size_t minimum) {
    static const std::u32string kPunctuation = U"、。！？，．";
    for (size_t index = window.size(); index > 0; --index) {
        if (kPunctuation.find(window[index - 1]) != std::u32string::npos) {
            return index >= minimum ? std::optional<size_t>(index) : std::nullopt;
        }
    }
    return std::nullopt;
}

bool ChunkedConversionEngine::ConvertCached(const std::u32string& reading,
                                            const std::u32string& context,
                                            std::u32string* converted, std::string* error) {
    const CacheKey key(reading, context);
    const auto cached = cache_.find(key);
    if (cached != cache_.end()) {
        *converted = cached->second;
        inference_timer::Note(InferenceStats::kCache);
        return true;
    }
    std::vector<std::u32string> candidates;
    if (!base_->Convert(reading, context, 1, &candidates, error)) return false;
    *converted = candidates.empty() ? reading : candidates.front();
    cacheOrder_.push_back(key);
    if (cacheOrder_.size() > kCacheLimit) {
        cache_.erase(cacheOrder_.front());
        cacheOrder_.pop_front();
    }
    cache_[key] = *converted;
    return true;
}

} // namespace iroha
