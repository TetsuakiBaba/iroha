#include "iroha/learning_store.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include "iroha/json.h"
#include "iroha/unicode.h"

namespace iroha {

namespace {

std::string FormatIso8601(int64_t unixTime) {
    const time_t t = static_cast<time_t>(unixTime);
    struct tm utc = {};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                  utc.tm_min, utc.tm_sec);
    return buf;
}

int64_t ParseIso8601(const std::string& text) {
    struct tm utc = {};
    // 小数秒は無視する（Swiftの.iso8601は秒精度で出力する）
#ifdef _WIN32
    const int fields = sscanf_s(text.c_str(), "%d-%d-%dT%d:%d:%d", &utc.tm_year, &utc.tm_mon,
                                &utc.tm_mday, &utc.tm_hour, &utc.tm_min, &utc.tm_sec);
#else
    const int fields = std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &utc.tm_year, &utc.tm_mon,
                                   &utc.tm_mday, &utc.tm_hour, &utc.tm_min, &utc.tm_sec);
#endif
    if (fields != 6) return 0;
    utc.tm_year -= 1900;
    utc.tm_mon -= 1;
#ifdef _WIN32
    return static_cast<int64_t>(_mkgmtime(&utc));
#else
    return static_cast<int64_t>(timegm(&utc));
#endif
}

std::vector<LearningEntry> Load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::stringstream buffer;
    buffer << file.rdbuf();
    json::Value root;
    if (!json::Parse(buffer.str(), &root)) return {};
    const json::Value* entriesValue = root.Find("entries");
    if (!entriesValue || entriesValue->type != json::Value::Type::Array) return {};

    std::vector<LearningEntry> entries;
    for (const json::Value& item : entriesValue->array) {
        const json::Value* reading = item.Find("reading");
        const json::Value* result = item.Find("result");
        if (!reading || !result) continue;
        // 旧形式の文節のエントリ（読みの一部）は捨てる。読み全体の学習として扱うと
        // 文中の一部を覚えた語が入力全体の変換結果になってしまう
        const json::Value* kind = item.Find("kind");
        if (kind && kind->AsString() == "segment") continue;
        LearningEntry entry;
        entry.reading = Utf8ToUtf32(reading->AsString());
        entry.result = Utf8ToUtf32(result->AsString());
        const json::Value* replaced = item.Find("replaced");
        if (replaced && replaced->type == json::Value::Type::String) {
            entry.replaced = Utf8ToUtf32(replaced->string);
        }
        if (const json::Value* updatedAt = item.Find("updatedAt")) {
            entry.updatedAt = ParseIso8601(updatedAt->AsString());
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace

LearningStore::LearningStore(std::filesystem::path path)
    : path_(std::move(path)), cached_(Load(path_)) {}

void LearningStore::Record(const std::u32string& reading,
                           const std::optional<std::u32string>& replaced,
                           const std::u32string& result) {
    if (reading.empty() || result.empty() || (replaced && *replaced == result)) return;
    LearningEntry recorded;
    recorded.reading = reading;
    recorded.replaced = replaced;
    recorded.result = result;
    recorded.updatedAt = static_cast<int64_t>(std::time(nullptr));

    std::vector<LearningEntry> base;
    for (const LearningEntry& entry : cached_.Entries()) {
        // 「直す前」を記録した修正は、同じ読みの記録なしの学習を置き換える
        if (replaced && entry.reading == reading && !entry.replaced) continue;
        base.push_back(entry);
    }
    Store(Merged(base, {recorded}));
}

void LearningStore::Forget(const std::u32string& reading, const std::u32string& engineResult,
                           const std::u32string& result) {
    const LearningEntry* applied = cached_.Entry(reading, &engineResult);
    if (!applied || applied->result != result) return;
    const LearningEntry target = *applied;
    std::vector<LearningEntry> entries;
    for (const LearningEntry& entry : cached_.Entries()) {
        if (!entry.SameKey(target)) entries.push_back(entry);
    }
    Store(std::move(entries));
}

void LearningStore::ReplaceAll(std::vector<LearningEntry> entries) {
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [](const LearningEntry& entry) {
                                     return entry.reading.empty() || entry.result.empty();
                                 }),
                  entries.end());
    Store(std::move(entries));
}

void LearningStore::Reset() {
    cached_ = LearningDictionary();
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

std::vector<LearningEntry> LearningStore::Merged(const std::vector<LearningEntry>& base,
                                                 const std::vector<LearningEntry>& recorded) {
    std::vector<LearningEntry> entries;
    const auto add = [&entries](const LearningEntry& entry) {
        for (LearningEntry& existing : entries) {
            if (!existing.SameKey(entry)) continue;
            // ユーザの今回の修正（recorded側）は同時刻でも優先する
            if (entry.updatedAt >= existing.updatedAt) existing = entry;
            return;
        }
        entries.push_back(entry);
    };
    for (const LearningEntry& entry : base) add(entry);
    for (const LearningEntry& entry : recorded) add(entry);

    if (entries.size() > kMaxEntries) {
        std::stable_sort(entries.begin(), entries.end(),
                         [](const LearningEntry& a, const LearningEntry& b) {
                             return a.updatedAt > b.updatedAt;
                         });
        entries.resize(kMaxEntries);
    }
    return entries;
}

void LearningStore::Store(std::vector<LearningEntry> entries) {
    Save(entries);
    cached_ = LearningDictionary(std::move(entries));
}

void LearningStore::Save(const std::vector<LearningEntry>& entries) const {
    // macOS版のJSONEncoder（.sortedKeys, .withoutEscapingSlashes, .iso8601）と同形式。
    // 文節単位の学習があった頃のキー（kind / leftContext）も、古い版が同じファイルを読めるように書く
    std::string out = "{\"entries\":[";
    for (size_t i = 0; i < entries.size(); ++i) {
        const LearningEntry& entry = entries[i];
        if (i > 0) out += ",";
        out += "{\"kind\":\"sentence\",\"leftContext\":\"\"";
        out += ",\"reading\":" + json::QuoteString(Utf32ToUtf8(entry.reading));
        if (entry.replaced) {
            out += ",\"replaced\":" + json::QuoteString(Utf32ToUtf8(*entry.replaced));
        }
        out += ",\"result\":" + json::QuoteString(Utf32ToUtf8(entry.result));
        out += ",\"updatedAt\":" + json::QuoteString(FormatIso8601(entry.updatedAt));
        out += "}";
    }
    out += "],\"version\":1}";

    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    const std::filesystem::path tmp = TemporaryPath(path_);
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) return;
        file.write(out.data(), static_cast<std::streamsize>(out.size()));
    }
    std::filesystem::rename(tmp, path_, ec);
}

} // namespace iroha
