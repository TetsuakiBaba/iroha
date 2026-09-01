#include "iroha/user_dictionary_store.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

#include "iroha/json.h"
#include "iroha/unicode.h"

namespace iroha {

namespace {

std::vector<UserDictionaryEntry> Load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::stringstream buffer;
    buffer << file.rdbuf();
    json::Value root;
    if (!json::Parse(buffer.str(), &root)) return {};
    const json::Value* entriesValue = root.Find("entries");
    if (!entriesValue || entriesValue->type != json::Value::Type::Array) return {};

    std::vector<UserDictionaryEntry> entries;
    for (const json::Value& item : entriesValue->array) {
        const json::Value* reading = item.Find("reading");
        const json::Value* word = item.Find("word");
        if (!reading || !word) continue;
        UserDictionaryEntry entry;
        entry.reading = Utf8ToUtf32(reading->AsString());
        entry.word = Utf8ToUtf32(word->AsString());
        if (const json::Value* id = item.Find("id")) entry.id = id->AsString();
        if (entry.id.empty()) entry.id = GenerateUuid();
        if (const json::Value* source = item.Find("source")) {
            entry.source = source->AsString() == "system"
                               ? UserDictionaryEntry::Source::System
                               : UserDictionaryEntry::Source::Manual;
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

using Pair = std::pair<std::u32string, std::u32string>; // (読み, 単語)の同一性判定用

} // namespace

UserDictionaryStore::UserDictionaryStore(std::filesystem::path path)
    : path_(std::move(path)), cached_(Load(path_)) {}

void UserDictionaryStore::ReplaceAll(std::vector<UserDictionaryEntry> entries) {
    Store(std::move(entries));
}

void UserDictionaryStore::Add(const std::u32string& reading,
                              const std::u32string& word) {
    std::vector<UserDictionaryEntry> entries = cached_.Entries();
    entries.push_back(
        MakeUserDictionaryEntry(reading, word, UserDictionaryEntry::Source::Manual));
    Store(std::move(entries));
}

UserDictionaryStore::SyncResult UserDictionaryStore::SyncFromSystem(
    const std::vector<std::pair<std::u32string, std::u32string>>& systemEntries) {
    std::vector<Pair> valid;
    int skipped = 0;
    for (const auto& [rawReading, rawWord] : systemEntries) {
        const std::u32string reading = UserDictionary::NormalizedReading(rawReading);
        const std::u32string word = TrimWhitespace(rawWord);
        if (!UserDictionary::IsImportableReading(reading) || word.empty()) {
            ++skipped;
            continue;
        }
        valid.push_back({reading, word});
    }

    const std::vector<UserDictionaryEntry>& existing = cached_.Entries();

    // 手動エントリと同じ内容が既にあるものは追加しない（重複表示を避ける）
    std::set<Pair> manualPairs;
    for (const UserDictionaryEntry& entry : existing) {
        if (entry.source == UserDictionaryEntry::Source::Manual) {
            manualPairs.insert({entry.reading, entry.word});
        }
    }
    std::set<Pair> wanted;
    std::vector<Pair> ordered;
    for (const Pair& pair : valid) {
        if (manualPairs.count(pair) || wanted.count(pair)) continue;
        wanted.insert(pair);
        ordered.push_back(pair);
    }

    SyncResult result;
    result.skipped = skipped;
    std::vector<UserDictionaryEntry> updated;
    std::set<Pair> kept;
    for (const UserDictionaryEntry& entry : existing) {
        if (entry.source != UserDictionaryEntry::Source::System) {
            updated.push_back(entry);
            continue;
        }
        const Pair pair{entry.reading, entry.word};
        if (wanted.count(pair) && !kept.count(pair)) {
            updated.push_back(entry);
            kept.insert(pair);
            ++result.unchanged;
        } else {
            ++result.removed;
        }
    }
    for (const Pair& pair : ordered) {
        if (kept.count(pair)) continue;
        updated.push_back(MakeUserDictionaryEntry(pair.first, pair.second,
                                                  UserDictionaryEntry::Source::System));
        ++result.added;
    }

    if (result.added > 0 || result.removed > 0) {
        Store(std::move(updated));
    }
    return result;
}

void UserDictionaryStore::Store(std::vector<UserDictionaryEntry> entries) {
    // 読みが空・単語が空のものは落とし、読み順に並べる（同じ読みの候補は登録順を保つ）
    std::vector<UserDictionaryEntry> cleaned;
    for (UserDictionaryEntry& entry : entries) {
        entry.reading = UserDictionary::NormalizedReading(entry.reading);
        entry.word = TrimWhitespace(entry.word);
        if (entry.reading.empty() || entry.word.empty()) continue;
        cleaned.push_back(std::move(entry));
    }
    std::stable_sort(cleaned.begin(), cleaned.end(),
                     [](const UserDictionaryEntry& a, const UserDictionaryEntry& b) {
                         return a.reading < b.reading;
                     });
    Save(cleaned);
    cached_ = UserDictionary(std::move(cleaned));
}

void UserDictionaryStore::Save(const std::vector<UserDictionaryEntry>& entries) const {
    // macOS版のJSONEncoder（.prettyPrinted, .sortedKeys, .withoutEscapingSlashes）と同形式
    std::string out = "{\n  \"entries\" : [\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        const UserDictionaryEntry& entry = entries[i];
        out += "    {\n";
        out += "      \"id\" : " + json::QuoteString(entry.id) + ",\n";
        out += "      \"reading\" : " + json::QuoteString(Utf32ToUtf8(entry.reading)) +
               ",\n";
        out += "      \"source\" : " +
               json::QuoteString(entry.source == UserDictionaryEntry::Source::System
                                     ? "system"
                                     : "manual") +
               ",\n";
        out += "      \"word\" : " + json::QuoteString(Utf32ToUtf8(entry.word)) + "\n";
        out += i + 1 < entries.size() ? "    },\n" : "    }\n";
    }
    out += "  ],\n  \"version\" : 1\n}";

    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    const std::filesystem::path tmp = path_.wstring() + L".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) return;
        file.write(out.data(), static_cast<std::streamsize>(out.size()));
    }
    std::filesystem::rename(tmp, path_, ec);
}

} // namespace iroha
