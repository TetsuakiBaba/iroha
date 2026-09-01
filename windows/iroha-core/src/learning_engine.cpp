#include "iroha/learning_engine.h"

#include <algorithm>

namespace iroha {

LearningEngine::LearningEngine(ConversionEngine* base, DictionaryProvider dictionary)
    : base_(base), dictionaryProvider_(std::move(dictionary)) {}

bool LearningEngine::Convert(const std::u32string& reading,
                             const std::u32string& context, int candidateCount,
                             std::vector<std::u32string>* results, std::string* error) {
    results->clear();
    const LearningDictionary dictionary = dictionaryProvider_();
    if (dictionary.IsEmpty()) {
        return base_->Convert(reading, context, candidateCount, results, error);
    }

    if (candidateCount <= 1) {
        // 入力全体を過去に確定していればそれをそのまま返す（エンジンを呼ばない）
        if (const std::u32string* sentence = dictionary.Sentence(reading)) {
            results->push_back(*sentence);
            return true;
        }
        std::u32string composed;
        if (!Compose(reading, context, dictionary, &composed, error)) return false;
        results->push_back(composed);
        return true;
    }

    // 候補ウィンドウ: この読みで学習済みの変換を先頭に、続けてエンジンの候補
    *results = dictionary.LearnedResults(reading, context);
    std::vector<std::u32string> baseCandidates;
    std::string baseError;
    if (base_->Convert(reading, context, candidateCount, &baseCandidates, &baseError)) {
        for (const auto& candidate : baseCandidates) {
            if (std::find(results->begin(), results->end(), candidate) ==
                results->end()) {
                results->push_back(candidate);
            }
        }
    } else if (results->empty()) {
        if (error) *error = baseError;
        return false;
    }
    return true;
}

// 学習済みの文節で埋めながら左から変換する。
// 文脈の条件を判定するには直前までの変換結果が要るので、学習済みの読みに
// ぶつかった時点で、そこまでの未変換部分を先にエンジンへ渡して確定させる
bool LearningEngine::Compose(const std::u32string& reading,
                             const std::u32string& context,
                             const LearningDictionary& dictionary, std::u32string* out,
                             std::string* error) {
    std::u32string result;  // 変換済みの部分（文脈にもなる）
    std::u32string pending; // まだエンジンに渡していない読み
    size_t index = 0;

    while (index < reading.size()) {
        if (dictionary.MayMatch(reading, index, index == 0)) {
            if (!pending.empty()) {
                std::u32string converted;
                if (!ConvertChunk(pending, context + result, &converted, error)) {
                    return false;
                }
                result += converted;
                pending.clear();
            }
            if (const LearningEntry* match =
                    dictionary.BestMatch(reading, index, result, index == 0)) {
                result += match->result;
                index += match->reading.size();
                continue;
            }
        }
        pending.push_back(reading[index]);
        ++index;
    }
    if (!pending.empty()) {
        std::u32string converted;
        if (!ConvertChunk(pending, context + result, &converted, error)) return false;
        result += converted;
    }
    *out = result;
    return true;
}

bool LearningEngine::ConvertChunk(const std::u32string& reading,
                                  const std::u32string& context, std::u32string* out,
                                  std::string* error) {
    std::vector<std::u32string> candidates;
    if (!base_->Convert(reading, context, 1, &candidates, error)) return false;
    *out = candidates.empty() ? reading : candidates.front();
    return true;
}

} // namespace iroha
