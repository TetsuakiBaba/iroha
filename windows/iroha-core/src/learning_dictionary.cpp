#include "iroha/learning_dictionary.h"

#include <algorithm>

namespace iroha {

LearningDictionary::LearningDictionary(std::vector<LearningEntry> entries)
    : entries_(std::move(entries)) {
    std::vector<const LearningEntry*> sorted;
    sorted.reserve(entries_.size());
    for (const LearningEntry& entry : entries_) sorted.push_back(&entry);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const LearningEntry* a, const LearningEntry* b) {
                         return a->updatedAt > b->updatedAt;
                     });
    for (const LearningEntry* entry : sorted) {
        if (entry->reading.empty() || entry->result.empty()) continue;
        std::vector<LearningEntry>& bucket = byReading_[entry->reading];
        // 同じキーなら新しい方を採る（並べ替え済みなので先勝ち）
        const bool seen = std::any_of(bucket.begin(), bucket.end(),
                                      [&](const LearningEntry& e) { return e.SameKey(*entry); });
        if (!seen) bucket.push_back(*entry);
    }
}

const std::vector<LearningEntry>& LearningDictionary::EntriesForReading(
    const std::u32string& reading) const {
    static const std::vector<LearningEntry> kEmpty;
    const auto it = byReading_.find(reading);
    return it == byReading_.end() ? kEmpty : it->second;
}

const LearningEntry* LearningDictionary::Entry(const std::u32string& reading,
                                               const std::u32string* engineResult) const {
    const std::vector<LearningEntry>& candidates = EntriesForReading(reading);
    if (engineResult) {
        for (const LearningEntry& entry : candidates) {
            if (entry.replaced && *entry.replaced == *engineResult) return &entry;
        }
    }
    for (const LearningEntry& entry : candidates) {
        if (!entry.replaced) return &entry;
    }
    return nullptr;
}

} // namespace iroha
