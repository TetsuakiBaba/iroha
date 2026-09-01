#include "iroha/learning_store.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>

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
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &utc.tm_year, &utc.tm_mon,
                    &utc.tm_mday, &utc.tm_hour, &utc.tm_min, &utc.tm_sec) != 6) {
        return 0;
    }
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
        const json::Value* kind = item.Find("kind");
        const json::Value* reading = item.Find("reading");
        const json::Value* result = item.Find("result");
        if (!kind || !reading || !result) continue;
        LearningEntry entry;
        entry.kind = kind->AsString() == "segment" ? LearningEntry::Kind::Segment
                                                   : LearningEntry::Kind::Sentence;
        entry.reading = Utf8ToUtf32(reading->AsString());
        entry.result = Utf8ToUtf32(result->AsString());
        if (const json::Value* context = item.Find("leftContext")) {
            entry.leftContext = Utf8ToUtf32(context->AsString());
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

void LearningStore::Record(const std::u32string& reading, const std::u32string& result,
                           const std::vector<SegmentPair>& segments) {
    if (reading.empty() || result.empty()) return;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    std::vector<LearningEntry> recorded;
    recorded.push_back(
        {LearningEntry::Kind::Sentence, reading, result, std::u32string(), now});
    // 文節は「直前までに確定した文字列」を文脈として一緒に覚える。
    // これで同じ読みでも位置によって違う変換を再現できる（記者の貴社）
    std::u32string leftContext;
    for (const SegmentPair& segment : segments) {
        if (!segment.reading.empty() && !segment.result.empty()) {
            recorded.push_back({LearningEntry::Kind::Segment, segment.reading,
                                segment.result, leftContext, now});
        }
        leftContext += segment.result;
        constexpr size_t kContextLength =
            static_cast<size_t>(LearningDictionary::kContextLength);
        if (leftContext.size() > kContextLength) {
            leftContext = leftContext.substr(leftContext.size() - kContextLength);
        }
    }
    Merge(recorded);
}

void LearningStore::Reset() {
    cached_ = LearningDictionary();
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

void LearningStore::Merge(const std::vector<LearningEntry>& recorded) {
    // 同じ (種類, 読み, 文脈) は新しいもので置き換える
    using Key = std::tuple<int, std::u32string, std::u32string>;
    auto keyOf = [](const LearningEntry& entry) {
        return Key{static_cast<int>(entry.kind), entry.reading,
                   entry.kind == LearningEntry::Kind::Sentence ? std::u32string()
                                                               : entry.leftContext};
    };
    std::map<Key, LearningEntry> byKey;
    std::vector<Key> order;
    for (const LearningEntry& entry : cached_.Entries()) {
        const Key key = keyOf(entry);
        if (byKey.find(key) == byKey.end()) order.push_back(key);
        byKey[key] = entry;
    }
    for (const LearningEntry& entry : recorded) {
        const Key key = keyOf(entry);
        if (byKey.find(key) == byKey.end()) order.push_back(key);
        byKey[key] = entry;
    }

    std::vector<LearningEntry> sentences;
    std::vector<LearningEntry> segments;
    for (const Key& key : order) {
        const LearningEntry& entry = byKey[key];
        (entry.kind == LearningEntry::Kind::Sentence ? sentences : segments)
            .push_back(entry);
    }
    // 上限を超えたら古いものから捨てる
    auto newestFirst = [](const LearningEntry& a, const LearningEntry& b) {
        return a.updatedAt > b.updatedAt;
    };
    if (sentences.size() > kMaxSentences) {
        std::stable_sort(sentences.begin(), sentences.end(), newestFirst);
        sentences.resize(kMaxSentences);
    }
    if (segments.size() > kMaxSegments) {
        std::stable_sort(segments.begin(), segments.end(), newestFirst);
        segments.resize(kMaxSegments);
    }
    std::vector<LearningEntry> entries = sentences;
    entries.insert(entries.end(), segments.begin(), segments.end());
    Save(entries);
    cached_ = LearningDictionary(std::move(entries));
}

void LearningStore::Save(const std::vector<LearningEntry>& entries) const {
    // macOS版のJSONEncoder（.sortedKeys, .withoutEscapingSlashes, .iso8601）と同形式
    std::string out = "{\"entries\":[";
    for (size_t i = 0; i < entries.size(); ++i) {
        const LearningEntry& entry = entries[i];
        if (i > 0) out += ",";
        out += "{\"kind\":";
        out += json::QuoteString(entry.kind == LearningEntry::Kind::Segment
                                     ? "segment"
                                     : "sentence");
        out += ",\"leftContext\":" + json::QuoteString(Utf32ToUtf8(entry.leftContext));
        out += ",\"reading\":" + json::QuoteString(Utf32ToUtf8(entry.reading));
        out += ",\"result\":" + json::QuoteString(Utf32ToUtf8(entry.result));
        out += ",\"updatedAt\":" + json::QuoteString(FormatIso8601(entry.updatedAt));
        out += "}";
    }
    out += "],\"version\":1}";

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
