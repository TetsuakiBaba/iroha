#include "iroha/learning_engine.h"
#include "iroha/inference_timer.h"

#include <algorithm>

namespace iroha {

namespace {

// 学習結果の列から重複を除く（最初に出た順）
std::vector<std::u32string> UniqueResults(const std::vector<LearningEntry>& entries) {
    std::vector<std::u32string> results;
    for (const LearningEntry& entry : entries) {
        if (std::find(results.begin(), results.end(), entry.result) == results.end()) {
            results.push_back(entry.result);
        }
    }
    return results;
}

} // namespace

LearningEngine::LearningEngine(ConversionEngine* base, DictionaryProvider dictionary)
    : base_(base), dictionaryProvider_(std::move(dictionary)) {}

bool LearningEngine::Convert(const std::u32string& reading,
                             const std::u32string& context, int candidateCount,
                             std::vector<std::u32string>* results, std::string* error) {
    if (!results) return false;
    const LearningDictionary dictionary = dictionaryProvider_();
    const std::vector<LearningEntry>& entries = dictionary.EntriesForReading(reading);
    if (entries.empty()) {
        if (candidateCount <= 1) recentDecisions_.erase(reading);
        return base_->Convert(reading, context, candidateCount, results, error);
    }

    std::vector<std::u32string> baseCandidates;
    std::string baseError;
    if (!base_->Convert(reading, context, candidateCount, &baseCandidates, &baseError)) {
        // エンジンが失敗したら、条件なしで差し替えられる学習結果だけで返す
        // （候補ウィンドウは学習結果が1件でもあれば開く）
        results->clear();
        if (candidateCount <= 1) {
            if (const LearningEntry* entry = dictionary.Entry(reading, nullptr)) {
                results->push_back(entry->result);
            }
        } else {
            *results = UniqueResults(entries);
        }
        if (results->empty()) {
            if (error) *error = baseError;
            return false;
        }
        inference_timer::Note(InferenceStats::kLearning);
        return true;
    }

    const std::u32string* engineFirst = baseCandidates.empty() ? nullptr : &baseCandidates.front();
    const LearningEntry* applied = dictionary.Entry(reading, engineFirst);

    if (candidateCount <= 1) {
        if (!applied && !engineFirst) {
            *results = std::move(baseCandidates);
            return true;
        }
        const std::u32string shown = applied ? applied->result : *engineFirst;
        if (engineFirst) {
            if (recentDecisions_.size() >= kMaxRecentDecisions) recentDecisions_.clear();
            recentDecisions_[reading] = {shown, *engineFirst};
        }
        results->assign(1, shown);
        return true;
    }

    // 候補ウィンドウ: 差し替える学習結果があれば先頭に、残りの学習結果はその次に置く
    *results = std::move(baseCandidates);
    if (applied) {
        results->erase(std::remove(results->begin(), results->end(), applied->result), results->end());
        results->insert(results->begin(), applied->result);
    }
    std::vector<std::u32string> others = UniqueResults(entries);
    if (!results->empty()) {
        const std::u32string first = results->front();
        others.erase(std::remove(others.begin(), others.end(), first), others.end());
    }
    results->erase(std::remove_if(results->begin(), results->end(),
                                  [&](const std::u32string& candidate) {
                                      return std::find(others.begin(), others.end(), candidate) !=
                                             others.end();
                                  }),
                   results->end());
    results->insert(results->begin() + std::min<size_t>(1, results->size()), others.begin(),
                    others.end());
    return true;
}

std::optional<std::u32string> LearningEngine::EngineResult(const std::u32string& reading,
                                                           const std::u32string& shown) const {
    const auto it = recentDecisions_.find(reading);
    if (it == recentDecisions_.end() || it->second.first != shown) return std::nullopt;
    return it->second.second;
}

} // namespace iroha
