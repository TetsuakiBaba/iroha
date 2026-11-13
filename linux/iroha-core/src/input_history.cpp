#include "iroha/input_history.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

#include "iroha/json.h"
#include "iroha/reading_aligner.h"
#include "iroha/unicode.h"

namespace iroha {

namespace {

bool IsHiragana(char32_t c) { return c >= 0x3041 && c <= 0x3096; }
bool IsReadingCharacter(char32_t c) { return c == U'ー' || IsHiragana(c); }
bool IsAlphanumeric(char32_t c) {
    return (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5A) || (c >= 0x61 && c <= 0x7A) ||
           (c >= 0xFF10 && c <= 0xFF19) || (c >= 0xFF21 && c <= 0xFF3A) || (c >= 0xFF41 && c <= 0xFF5A);
}
bool IsTrailingPunctuation(char32_t c) {
    return c == U'、' || c == U'。' || c == U'，' || c == U'．' || c == U'！' || c == U'？' || c == U'・';
}

bool RankedBefore(const InputHistoryEntry& lhs, const InputHistoryEntry& rhs) {
    if (lhs.count != rhs.count) return lhs.count > rhs.count;
    if (lhs.lastUsed != rhs.lastUsed) return lhs.lastUsed > rhs.lastUsed;
    return lhs.reading < rhs.reading;
}

bool IsCounted(const InputHistoryEntry& entry, int64_t clearedAt,
               const std::map<InputHistoryKey, int64_t>& removedAt) {
    const auto removed = removedAt.find({entry.reading, entry.text});
    return entry.lastUsed > clearedAt && (removed == removedAt.end() || entry.lastUsed > removed->second);
}

// 上限を超えた分を、最後に使った日時が古いものから捨てる
void Trim(InputHistoryStore::HostFile* file) {
    if (file->entries.size() <= InputHistoryStore::kMaxEntries) return;
    std::sort(file->entries.begin(), file->entries.end(),
              [](const InputHistoryEntry& a, const InputHistoryEntry& b) { return a.lastUsed > b.lastUsed; });
    file->entries.resize(InputHistoryStore::kMaxEntries);
}

constexpr int64_t kDistantPast = INT64_MIN / 2;

} // namespace

// ---- 単位の切り出し ----

std::vector<InputHistoryKey> InputHistoryUnits::Units(const std::u32string& reading, const std::u32string& text) {
    std::vector<InputHistoryKey> units;
    if (reading.empty() || text.empty()) return units;
    const auto add = [&units](const std::u32string& unitReading, const std::u32string& unitText) {
        if (unitReading.size() < kMinimumReadingLength || unitText.empty() ||
            !std::all_of(unitReading.begin(), unitReading.end(), IsReadingCharacter) ||
            std::any_of(unitText.begin(), unitText.end(), IsAlphanumeric)) {
            return;
        }
        const InputHistoryKey key{unitReading, unitText};
        if (std::find(units.begin(), units.end(), key) == units.end()) units.push_back(key);
    };
    for (const auto& segment : ReadingAligner::SegmentReading(reading, text)) {
        std::u32string segmentReading = segment.reading;
        std::u32string segmentText = segment.conversion;
        while (!segmentText.empty() && IsTrailingPunctuation(segmentText.back()) && !segmentReading.empty() &&
               segmentReading.back() == segmentText.back()) {
            segmentText.pop_back();
            segmentReading.pop_back();
        }
        add(segmentReading, segmentText);
        // 語の部分: 表記の末尾のひらがな（送り仮名・助詞）を読みからも同じだけ除く
        if (std::all_of(segmentText.begin(), segmentText.end(), IsHiragana)) continue;
        size_t suffixLength = 0;
        while (suffixLength < segmentText.size() && IsHiragana(segmentText[segmentText.size() - 1 - suffixLength])) {
            ++suffixLength;
        }
        if (suffixLength == 0 || segmentReading.size() < suffixLength ||
            segmentReading.compare(segmentReading.size() - suffixLength, suffixLength,
                                   segmentText, segmentText.size() - suffixLength, suffixLength) != 0) {
            continue;
        }
        add(segmentReading.substr(0, segmentReading.size() - suffixLength),
            segmentText.substr(0, segmentText.size() - suffixLength));
    }
    return units;
}

// ---- 日時 ----

int64_t InputHistoryStore::NowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string InputHistoryStore::FormatTime(int64_t seconds) {
    const int64_t days = seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400;
    const int64_t rest = seconds - days * 86400;
    // 1970-01-01 からの日数 → 年月日
    int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    char text[32];
    std::snprintf(text, sizeof(text), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ", static_cast<long long>(year),
                  static_cast<long long>(month), static_cast<long long>(day),
                  static_cast<long long>(rest / 3600), static_cast<long long>(rest % 3600 / 60),
                  static_cast<long long>(rest % 60));
    return text;
}

std::optional<int64_t> InputHistoryStore::ParseTime(const std::string& text) {
    // 「YYYY-MM-DDTHH:MM:SS」の決まった位置の数字を読む（小数秒・時間帯の後ろは見ない）
    if (text.size() < 19 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
        text[16] != ':') {
        return std::nullopt;
    }
    const auto field = [&text](size_t start, size_t length, int* value) {
        *value = 0;
        for (size_t i = start; i < start + length; ++i) {
            if (text[i] < '0' || text[i] > '9') return false;
            *value = *value * 10 + (text[i] - '0');
        }
        return true;
    };
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!field(0, 4, &year) || !field(5, 2, &month) || !field(8, 2, &day) || !field(11, 2, &hour) ||
        !field(14, 2, &minute) || !field(17, 2, &second)) {
        return std::nullopt;
    }
    // 年月日 → 1970-01-01 からの日数（時間帯の指定は Z（UTC）だけを書くので、ほかは見ない）
    const int y = year - (month <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

// ---- 保存と読み込み ----

namespace {

std::string EncodeHostFile(const InputHistoryStore::HostFile& file) {
    // macOS版の JSONEncoder（sortedKeys）と同じくキーの順に並べる
    std::string out = "{";
    if (file.clearedAt) out += "\"clearedAt\":" + json::QuoteString(InputHistoryStore::FormatTime(*file.clearedAt)) + ",";
    out += "\"entries\":[";
    for (size_t i = 0; i < file.entries.size(); ++i) {
        const InputHistoryEntry& entry = file.entries[i];
        if (i > 0) out += ",";
        out += "{\"count\":" + std::to_string(entry.count) +
               ",\"lastUsed\":" + json::QuoteString(InputHistoryStore::FormatTime(entry.lastUsed)) +
               ",\"reading\":" + json::QuoteString(Utf32ToUtf8(entry.reading)) +
               ",\"text\":" + json::QuoteString(Utf32ToUtf8(entry.text)) + "}";
    }
    out += "]";
    if (file.importedThrough) {
        out += ",\"importedThrough\":" + json::QuoteString(InputHistoryStore::FormatTime(*file.importedThrough));
    }
    out += ",\"removals\":[";
    for (size_t i = 0; i < file.removals.size(); ++i) {
        const auto& removal = file.removals[i];
        if (i > 0) out += ",";
        out += "{\"reading\":" + json::QuoteString(Utf32ToUtf8(removal.reading)) +
               ",\"removedAt\":" + json::QuoteString(InputHistoryStore::FormatTime(removal.removedAt)) +
               ",\"text\":" + json::QuoteString(Utf32ToUtf8(removal.text)) + "}";
    }
    out += "],\"version\":1}";
    return out;
}

} // namespace

InputHistoryStore::HostFile InputHistoryStore::Load(const std::filesystem::path& path) {
    HostFile file;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return file;
    std::stringstream buffer;
    buffer << stream.rdbuf();
    json::Value root;
    if (!json::Parse(buffer.str(), &root) || root.type != json::Value::Type::Object) return file;
    const auto time = [](const json::Value* value) -> std::optional<int64_t> {
        return value ? ParseTime(value->AsString()) : std::nullopt;
    };
    file.clearedAt = time(root.Find("clearedAt"));
    file.importedThrough = time(root.Find("importedThrough"));
    if (const json::Value* entries = root.Find("entries"); entries && entries->type == json::Value::Type::Array) {
        for (const json::Value& item : entries->array) {
            InputHistoryEntry entry;
            entry.reading = Utf8ToUtf32(item.Find("reading") ? item.Find("reading")->AsString() : "");
            entry.text = Utf8ToUtf32(item.Find("text") ? item.Find("text")->AsString() : "");
            const json::Value* count = item.Find("count");
            entry.count = count && count->type == json::Value::Type::Number ? static_cast<int64_t>(count->number) : 1;
            const auto lastUsed = time(item.Find("lastUsed"));
            if (entry.reading.empty() || entry.text.empty() || !lastUsed) continue;
            entry.lastUsed = *lastUsed;
            file.entries.push_back(std::move(entry));
        }
    }
    if (const json::Value* removals = root.Find("removals"); removals && removals->type == json::Value::Type::Array) {
        for (const json::Value& item : removals->array) {
            Removal removal;
            removal.reading = Utf8ToUtf32(item.Find("reading") ? item.Find("reading")->AsString() : "");
            removal.text = Utf8ToUtf32(item.Find("text") ? item.Find("text")->AsString() : "");
            const auto removedAt = time(item.Find("removedAt"));
            if (!removedAt) continue;
            removal.removedAt = *removedAt;
            file.removals.push_back(std::move(removal));
        }
    }
    return file;
}

void InputHistoryStore::Save(const HostFile& file, const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const std::string content = EncodeHostFile(file);
    const std::filesystem::path temporary = TemporaryPath(path);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return;
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    std::filesystem::rename(temporary, path, error);
}

// ---- ストア ----

InputHistoryStore::InputHistoryStore(std::filesystem::path directory, std::string hostName, int saveDelayMs)
    : directory_(std::move(directory)), hostName_(std::move(hostName)), saveDelayMs_(saveDelayMs) {
    Reload();
    if (saveDelayMs_ > 0) saver_ = std::thread([this] { SaverLoop(); });
}

InputHistoryStore::~InputHistoryStore() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    saveCondition_.notify_all();
    if (saver_.joinable()) saver_.join();
    // 残っている書き込みを済ませる
    bool dirty = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dirty = dirty_;
    }
    if (dirty) SaveNow();
}

std::filesystem::path InputHistoryStore::FilePath(const std::string& host) const {
    return directory_ / PathFromUtf8(host + ".json");
}

void InputHistoryStore::Reload() {
    HostFile own = Load(FilePath(hostName_));
    std::map<std::string, HostFile> others;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory_, error), end; !error && it != end; it.increment(error)) {
        if (it->path().extension() != ".json") continue;
        const std::string host = it->path().stem().u8string();
        if (host == hostName_) continue;
        others[host] = Load(it->path());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // 書き込み前の自分の分は手元のほうが新しい
    if (!dirty_) own_ = std::move(own);
    others_ = std::move(others);
    RebuildMerged();
}

// 全ホストを合算し直す（mutex_ を持った状態で呼ぶ）
void InputHistoryStore::RebuildMerged() {
    std::vector<const HostFile*> files = {&own_};
    for (const auto& item : others_) files.push_back(&item.second);
    int64_t clearedAt = kDistantPast;
    std::map<InputHistoryKey, int64_t> removedAt;
    for (const HostFile* file : files) {
        if (file->clearedAt) clearedAt = std::max(clearedAt, *file->clearedAt);
        for (const Removal& removal : file->removals) {
            int64_t& at = removedAt.emplace(InputHistoryKey{removal.reading, removal.text}, kDistantPast).first->second;
            at = std::max(at, removal.removedAt);
        }
    }
    merged_.clear();
    for (const HostFile* file : files) {
        for (const InputHistoryEntry& entry : file->entries) {
            if (!IsCounted(entry, clearedAt, removedAt)) continue;
            const InputHistoryKey key{entry.reading, entry.text};
            const auto found = merged_.find(key);
            if (found == merged_.end()) {
                merged_.emplace(key, entry);
            } else {
                found->second.count += entry.count;
                found->second.lastUsed = std::max(found->second.lastUsed, entry.lastUsed);
            }
        }
    }
    byFirstCharacter_.clear();
    for (const auto& item : merged_) byFirstCharacter_[item.second.reading.front()].push_back(item.second);
}

void InputHistoryStore::Record(const std::u32string& reading, const std::u32string& text, int64_t now) {
    const std::vector<InputHistoryKey> units = InputHistoryUnits::Units(reading, text);
    if (units.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const InputHistoryKey& key : units) {
            const auto found = std::find_if(own_.entries.begin(), own_.entries.end(),
                                            [&key](const InputHistoryEntry& entry) {
                                                return entry.reading == key.reading && entry.text == key.text;
                                            });
            if (found != own_.entries.end()) {
                ++found->count;
                found->lastUsed = std::max(found->lastUsed, now);
            } else {
                own_.entries.push_back({key.reading, key.text, 1, now});
            }
        }
        Trim(&own_);
        RebuildMerged();
    }
    ScheduleSave();
}

std::vector<InputHistoryEntry> InputHistoryStore::Candidates(const std::u32string& prefix, size_t limit) const {
    std::vector<InputHistoryEntry> matches;
    if (prefix.empty() || limit == 0) return matches;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto bucket = byFirstCharacter_.find(prefix.front());
        if (bucket == byFirstCharacter_.end()) return matches;
        for (const InputHistoryEntry& entry : bucket->second) {
            if (entry.reading.size() > prefix.size() && entry.reading.compare(0, prefix.size(), prefix) == 0) {
                matches.push_back(entry);
            }
        }
    }
    std::sort(matches.begin(), matches.end(), RankedBefore);
    std::vector<InputHistoryEntry> results;
    std::set<std::u32string> seen;
    for (const InputHistoryEntry& entry : matches) {
        if (!seen.insert(entry.text).second) continue;
        results.push_back(entry);
        if (results.size() >= limit) break;
    }
    return results;
}

std::vector<InputHistoryEntry> InputHistoryStore::Entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<InputHistoryEntry> entries;
    for (const auto& item : merged_) entries.push_back(item.second);
    return entries;
}

size_t InputHistoryStore::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return merged_.size();
}

void InputHistoryStore::Remove(const std::vector<InputHistoryKey>& keys, int64_t now) {
    if (keys.empty()) return;
    const std::set<InputHistoryKey> removing(keys.begin(), keys.end());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        own_.entries.erase(std::remove_if(own_.entries.begin(), own_.entries.end(),
                                          [&removing](const InputHistoryEntry& entry) {
                                              return removing.count({entry.reading, entry.text}) != 0;
                                          }),
                           own_.entries.end());
        own_.removals.erase(std::remove_if(own_.removals.begin(), own_.removals.end(),
                                           [&removing](const Removal& removal) {
                                               return removing.count({removal.reading, removal.text}) != 0;
                                           }),
                            own_.removals.end());
        for (const InputHistoryKey& key : keys) own_.removals.push_back({key.reading, key.text, now});
        if (own_.removals.size() > kMaxRemovals) {
            std::sort(own_.removals.begin(), own_.removals.end(),
                      [](const Removal& a, const Removal& b) { return a.removedAt > b.removedAt; });
            own_.removals.resize(kMaxRemovals);
        }
        RebuildMerged();
        dirty_ = true;
    }
    SaveNow();
}

void InputHistoryStore::Reset(int64_t now) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        own_.entries.clear();
        own_.removals.clear();
        own_.clearedAt = now;
        RebuildMerged();
        dirty_ = true;
    }
    SaveNow();
}

void InputHistoryStore::ScheduleSave() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dirty_ = true;
    }
    if (saveDelayMs_ <= 0) {
        SaveNow();
        return;
    }
    saveCondition_.notify_all();
}

void InputHistoryStore::Flush() {
    bool dirty = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dirty = dirty_;
    }
    if (dirty) SaveNow();
}

void InputHistoryStore::SaveNow() {
    HostFile contents;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        contents = own_;
        dirty_ = false;
    }
    Save(contents, FilePath(hostName_));
}

// 書き込みを遅らせる: 書くものができたら saveDelayMs_ 待ち、そのあいだの記録をまとめて 1 回で書く
void InputHistoryStore::SaverLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        saveCondition_.wait(lock, [this] { return stopping_ || dirty_; });
        if (stopping_) break;
        saveCondition_.wait_for(lock, std::chrono::milliseconds(saveDelayMs_), [this] { return stopping_; });
        if (stopping_) break;
        if (!dirty_) continue;
        HostFile contents = own_;
        dirty_ = false;
        lock.unlock();
        Save(contents, FilePath(hostName_));
        lock.lock();
    }
}

} // namespace iroha
