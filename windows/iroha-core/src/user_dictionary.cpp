#include "iroha/user_dictionary.h"

#include <algorithm>
#include <cstdio>
#include <random>

#include "iroha/kana.h"
#include "iroha/unicode.h"

namespace iroha {

std::string GenerateUuid() {
    static std::mt19937_64 rng{std::random_device{}()};
    const uint64_t high = rng();
    const uint64_t low = rng();
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08X-%04X-4%03X-%04X-%012llX",
                  static_cast<unsigned>(high >> 32),
                  static_cast<unsigned>((high >> 16) & 0xFFFF),
                  static_cast<unsigned>(high & 0xFFF),
                  static_cast<unsigned>(0x8000 | ((low >> 48) & 0x3FFF)),
                  static_cast<unsigned long long>(low & 0xFFFFFFFFFFFFULL));
    return buf;
}

UserDictionaryEntry MakeUserDictionaryEntry(const std::u32string& reading,
                                            const std::u32string& word,
                                            UserDictionaryEntry::Source source) {
    UserDictionaryEntry entry;
    entry.id = GenerateUuid();
    entry.reading = UserDictionary::NormalizedReading(reading);
    entry.word = word;
    entry.source = source;
    return entry;
}

UserDictionary::UserDictionary(std::vector<UserDictionaryEntry> entries)
    : entries_(std::move(entries)) {
    for (const UserDictionaryEntry& entry : entries_) {
        const std::u32string reading = NormalizedReading(entry.reading);
        if (reading.empty() || entry.word.empty()) continue;
        std::vector<std::u32string>& words = wordsByReading_[reading];
        if (std::find(words.begin(), words.end(), entry.word) != words.end()) continue;
        words.push_back(entry.word);
        maxReadingLength_ = std::max(maxReadingLength_, reading.size());
    }
    // 索引に何も入らなかった読みのキーを消す（emptyの判定を単純に保つ）
    for (auto it = wordsByReading_.begin(); it != wordsByReading_.end();) {
        it = it->second.empty() ? wordsByReading_.erase(it) : std::next(it);
    }
}

std::vector<std::u32string> UserDictionary::Words(const std::u32string& reading) const {
    auto it = wordsByReading_.find(NormalizedReading(reading));
    return it != wordsByReading_.end() ? it->second : std::vector<std::u32string>{};
}

std::vector<UserDictionary::Chunk> UserDictionary::Split(const std::u32string& reading,
                                                         int minimumMatchLength) const {
    if (IsEmpty() || reading.empty()) return {Chunk{false, reading}};
    std::vector<Chunk> chunks;
    std::u32string plain;
    size_t index = 0;

    while (index < reading.size()) {
        size_t matchLength = 0;
        const std::u32string* matchWord = nullptr;
        size_t length = std::min(maxReadingLength_, reading.size() - index);
        while (length >= static_cast<size_t>(minimumMatchLength)) {
            auto it = wordsByReading_.find(reading.substr(index, length));
            if (it != wordsByReading_.end() && !it->second.empty()) {
                matchLength = length;
                matchWord = &it->second.front();
                break;
            }
            --length;
        }
        if (matchWord) {
            if (!plain.empty()) {
                chunks.push_back(Chunk{false, plain});
                plain.clear();
            }
            chunks.push_back(Chunk{true, *matchWord});
            index += matchLength;
        } else {
            plain.push_back(reading[index]);
            ++index;
        }
    }
    if (!plain.empty()) chunks.push_back(Chunk{false, plain});
    return chunks;
}

std::u32string UserDictionary::NormalizedReading(const std::u32string& text) {
    return KatakanaToHiragana(TrimWhitespace(text));
}

bool UserDictionary::IsImportableReading(const std::u32string& text) {
    const std::u32string reading = NormalizedReading(text);
    if (reading.empty()) return false;
    return std::all_of(reading.begin(), reading.end(), [](char32_t c) {
        // ぁ〜ゖ・ゝゞ + 長音符
        return (c >= 0x3041 && c <= 0x3096) || (c >= 0x309D && c <= 0x309E) ||
               c == 0x30FC;
    });
}

} // namespace iroha
