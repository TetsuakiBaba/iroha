#include "iroha/learning_dictionary.h"

#include <algorithm>

namespace iroha {

namespace {

bool HasSuffix(const std::u32string& text, const std::u32string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

LearningDictionary::LearningDictionary(std::vector<LearningEntry> entries)
    : entries_(std::move(entries)) {
    std::vector<const LearningEntry*> sorted;
    sorted.reserve(entries_.size());
    for (const LearningEntry& entry : entries_) sorted.push_back(&entry);
    // 新しい順（同時刻は元の並びを保つ）
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const LearningEntry* a, const LearningEntry* b) {
                         return a->updatedAt > b->updatedAt;
                     });
    for (const LearningEntry* entry : sorted) {
        if (entry->reading.empty() || entry->result.empty()) continue;
        switch (entry->kind) {
            case LearningEntry::Kind::Sentence:
                // 同じ読みなら新しい方を採る（並べ替え済みなので先勝ち）
                sentences_.emplace(entry->reading, *entry);
                break;
            case LearningEntry::Kind::Segment:
                segments_[entry->reading].push_back(*entry);
                maxSegmentReadingLength_ =
                    std::max(maxSegmentReadingLength_, entry->reading.size());
                break;
        }
    }
}

const std::u32string* LearningDictionary::Sentence(const std::u32string& reading) const {
    auto it = sentences_.find(reading);
    return it != sentences_.end() ? &it->second.result : nullptr;
}

bool LearningDictionary::MayMatch(const std::u32string& characters, size_t index,
                                  bool atStart) const {
    // 位置indexから始まる読みを長い方から順に調べる
    size_t length = std::min(maxSegmentReadingLength_, characters.size() - index);
    while (length >= static_cast<size_t>(kMinimumMatchLength)) {
        auto it = segments_.find(characters.substr(index, length));
        if (it != segments_.end()) {
            // 文脈なしで覚えたものは入力の先頭でしか使わない
            const bool usable = std::any_of(
                it->second.begin(), it->second.end(),
                [atStart](const LearningEntry& e) {
                    return !e.leftContext.empty() || atStart;
                });
            if (usable) return true;
        }
        --length;
    }
    return false;
}

const LearningEntry* LearningDictionary::BestMatch(const std::u32string& characters,
                                                   size_t index,
                                                   const std::u32string& leftContext,
                                                   bool atStart) const {
    size_t length = std::min(maxSegmentReadingLength_, characters.size() - index);
    while (length >= static_cast<size_t>(kMinimumMatchLength)) {
        auto it = segments_.find(characters.substr(index, length));
        if (it != segments_.end()) {
            // 新しい順に見て、最初に文脈が合ったものを採る
            for (const LearningEntry& entry : it->second) {
                if (Applies(entry, leftContext, atStart)) return &entry;
            }
        }
        --length;
    }
    return nullptr;
}

bool LearningDictionary::Applies(const LearningEntry& entry,
                                 const std::u32string& leftContext,
                                 bool atStart) const {
    return entry.leftContext.empty() ? atStart
                                     : HasSuffix(leftContext, entry.leftContext);
}

std::vector<std::u32string> LearningDictionary::LearnedResults(
    const std::u32string& reading, const std::u32string& leftContext) const {
    std::vector<std::u32string> applicable;
    std::vector<std::u32string> others;
    auto it = segments_.find(reading);
    if (it != segments_.end()) {
        for (const LearningEntry& entry : it->second) {
            // 候補ウィンドウは明示的な操作なので、文脈が合わないものも後ろに残す
            if (Applies(entry, leftContext, leftContext.empty())) {
                applicable.push_back(entry.result);
            } else {
                others.push_back(entry.result);
            }
        }
    }
    auto sentence = sentences_.find(reading);
    if (sentence != sentences_.end()) {
        applicable.insert(applicable.begin(), sentence->second.result);
    }
    std::vector<std::u32string> results;
    for (const auto& list : {applicable, others}) {
        for (const auto& result : list) {
            if (std::find(results.begin(), results.end(), result) == results.end()) {
                results.push_back(result);
            }
        }
    }
    return results;
}

} // namespace iroha
