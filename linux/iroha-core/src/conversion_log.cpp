#include "iroha/conversion_log.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>

#include "iroha/input_history.h"
#include "iroha/json.h"
#include "iroha/unicode.h"

namespace iroha {

namespace {

const char* ModeName(ConversionLogEntry::Mode mode) {
    switch (mode) {
    case ConversionLogEntry::Mode::Segments:
        return "segments";
    case ConversionLogEntry::Mode::FunctionKey:
        return "functionKey";
    default:
        return "live";
    }
}

const char* SourceName(ConversionLogEntry::ContextSource source) {
    switch (source) {
    case ConversionLogEntry::ContextSource::Document:
        return "document";
    case ConversionLogEntry::ContextSource::Committed:
        return "committed";
    default:
        return "none";
    }
}

std::string Quote(const std::u32string& text) { return json::QuoteString(Utf32ToUtf8(text)); }

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    std::ifstream stream(path, std::ios::binary);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

void WriteLines(const std::vector<std::string>& lines, const std::filesystem::path& path) {
    std::error_code error;
    if (lines.empty()) {
        std::filesystem::remove(path, error);
        return;
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        for (const std::string& line : lines) stream << line << '\n';
        if (!stream) return;
    }
    std::filesystem::rename(temporary, path, error);
}

} // namespace

ConversionLogEntry ConversionLogEntry::Make(Mode mode, std::u32string context, ContextSource source,
                                            std::u32string reading, std::optional<std::u32string> proposed,
                                            std::u32string committed, std::optional<std::vector<Segment>> segments) {
    ConversionLogEntry entry;
    entry.timestamp = InputHistoryStore::NowSeconds();
    entry.mode = mode;
    entry.context = std::move(context);
    entry.contextSource = source;
    entry.reading = std::move(reading);
    entry.committed = std::move(committed);
    if (proposed) entry.edited = *proposed != entry.committed;
    entry.proposed = std::move(proposed);
    entry.segments = std::move(segments);
    return entry;
}

bool ConversionLogEntry::IsTrivial() const {
    return GraphemeCount(committed) == 1 && committed == reading && edited == false;
}

std::string ConversionLogEntry::Encode() const {
    std::string out = "{\"committed\":" + Quote(committed) + ",\"context\":" + Quote(context) +
                      ",\"contextSource\":\"" + SourceName(contextSource) + "\"";
    if (edited) out += std::string(",\"edited\":") + (*edited ? "true" : "false");
    out += ",\"format\":" + std::to_string(format) + ",\"mode\":\"" + ModeName(mode) + "\"" +
           ",\"model\":" + json::QuoteString(model);
    if (proposed) out += ",\"proposed\":" + Quote(*proposed);
    out += ",\"reading\":" + Quote(reading);
    if (segments) {
        out += ",\"segments\":[";
        for (size_t i = 0; i < segments->size(); ++i) {
            if (i > 0) out += ",";
            out += "{\"reading\":" + Quote((*segments)[i].reading) + ",\"result\":" + Quote((*segments)[i].result) +
                   "}";
        }
        out += "]";
    }
    out += ",\"timestamp\":" + json::QuoteString(InputHistoryStore::FormatTime(timestamp)) + "}";
    return out;
}

std::optional<ConversionLogEntry> ConversionLogEntry::Decode(const std::string& line) {
    json::Value value;
    if (!json::Parse(line, &value) || value.type != json::Value::Type::Object) return std::nullopt;
    const auto text = [&value](const char* key) -> const json::Value* {
        const json::Value* field = value.Find(key);
        return field && field->type == json::Value::Type::String ? field : nullptr;
    };
    const json::Value* committed = text("committed");
    const json::Value* context = text("context");
    const json::Value* source = text("contextSource");
    const json::Value* mode = text("mode");
    const json::Value* model = text("model");
    const json::Value* reading = text("reading");
    const json::Value* timestamp = text("timestamp");
    const json::Value* format = value.Find("format");
    if (!committed || !context || !source || !mode || !model || !reading || !timestamp || !format ||
        format->type != json::Value::Type::Number) {
        return std::nullopt;
    }
    ConversionLogEntry entry;
    entry.format = static_cast<int>(format->number);
    const auto time = InputHistoryStore::ParseTime(timestamp->string);
    if (!time) return std::nullopt;
    entry.timestamp = *time;
    if (mode->string == "live") {
        entry.mode = Mode::Live;
    } else if (mode->string == "segments") {
        entry.mode = Mode::Segments;
    } else if (mode->string == "functionKey") {
        entry.mode = Mode::FunctionKey;
    } else {
        return std::nullopt;
    }
    if (source->string == "document") {
        entry.contextSource = ContextSource::Document;
    } else if (source->string == "committed") {
        entry.contextSource = ContextSource::Committed;
    } else if (source->string == "none") {
        entry.contextSource = ContextSource::None;
    } else {
        return std::nullopt;
    }
    entry.committed = Utf8ToUtf32(committed->string);
    entry.context = Utf8ToUtf32(context->string);
    entry.reading = Utf8ToUtf32(reading->string);
    entry.model = model->string;
    if (const json::Value* proposed = text("proposed")) entry.proposed = Utf8ToUtf32(proposed->string);
    if (const json::Value* edited = value.Find("edited"); edited && edited->type == json::Value::Type::Bool) {
        entry.edited = edited->boolean;
    }
    if (const json::Value* segments = value.Find("segments");
        segments && segments->type == json::Value::Type::Array) {
        std::vector<Segment> list;
        for (const json::Value& item : segments->array) {
            const json::Value* segmentReading = item.Find("reading");
            const json::Value* result = item.Find("result");
            if (!segmentReading || !result) return std::nullopt;
            list.push_back({Utf8ToUtf32(segmentReading->AsString()), Utf8ToUtf32(result->AsString())});
        }
        entry.segments = std::move(list);
    }
    return entry;
}

bool ConversionLogEntry::operator==(const ConversionLogEntry& other) const {
    return format == other.format && timestamp == other.timestamp && mode == other.mode &&
           context == other.context && contextSource == other.contextSource && reading == other.reading &&
           proposed == other.proposed && committed == other.committed && edited == other.edited &&
           segments == other.segments && model == other.model;
}

bool ShouldRecordConversion(const ConversionLogEntry& entry, ConversionLogScope scope) {
    if (entry.context.empty() || entry.IsTrivial()) return false;
    return scope == ConversionLogScope::All || entry.edited != false;
}

// ---- ログファイル ----

ConversionLog::ConversionLog(std::filesystem::path directory, std::string hostName)
    : directory_(std::move(directory)), hostName_(std::move(hostName)) {}

std::filesystem::path ConversionLog::FilePath(int64_t timestamp) const {
    const std::time_t time = static_cast<std::time_t>(timestamp);
    std::tm local{};
    localtime_r(&time, &local);
    char month[16];
    std::snprintf(month, sizeof(month), "%04d-%02d", local.tm_year + 1900, local.tm_mon + 1);
    return directory_ / PathFromUtf8("conversions-" + hostName_ + "-" + month + ".jsonl");
}

void ConversionLog::Record(const ConversionLogEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    std::ofstream stream(FilePath(entry.timestamp), std::ios::binary | std::ios::app);
    stream << entry.Encode() << '\n';
}

std::vector<std::filesystem::path> ConversionLog::Files() const {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory_, error), end; !error && it != end;
         it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (name.rfind("conversions-", 0) == 0 && name.size() > 6 &&
            name.compare(name.size() - 6, 6, ".jsonl") == 0) {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

uintmax_t ConversionLog::TotalSize() const {
    uintmax_t total = 0;
    for (const auto& file : Files()) {
        std::error_code error;
        const uintmax_t size = std::filesystem::file_size(file, error);
        if (!error) total += size;
    }
    return total;
}

std::vector<ConversionLog::Item> ConversionLog::Items() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Item> items;
    for (const auto& file : Files()) {
        const std::vector<std::string> lines = ReadLines(file);
        for (size_t i = 0; i < lines.size(); ++i) {
            if (auto entry = ConversionLogEntry::Decode(lines[i])) {
                items.push_back(Item{file, i, lines[i], std::move(*entry)});
            }
        }
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& lhs, const Item& rhs) {
        if (lhs.entry.timestamp != rhs.entry.timestamp) return lhs.entry.timestamp < rhs.entry.timestamp;
        if (lhs.file.filename() != rhs.file.filename()) return lhs.file.filename() < rhs.file.filename();
        return lhs.line < rhs.line;
    });
    return items;
}

void ConversionLog::Delete(const std::vector<Item>& items) {
    if (items.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::filesystem::path, std::vector<const Item*>> byFile;
    for (const Item& item : items) byFile[item.file].push_back(&item);
    for (auto& [file, group] : byFile) {
        std::vector<std::string> lines = ReadLines(file);
        // 行番号の大きい方から消して、前の行の位置がずれないようにする
        std::sort(group.begin(), group.end(), [](const Item* a, const Item* b) { return a->line > b->line; });
        for (const Item* item : group) {
            if (item->line < lines.size() && lines[item->line] == item->original) {
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(item->line));
            } else if (const auto it = std::find(lines.begin(), lines.end(), item->original); it != lines.end()) {
                lines.erase(it);
            }
        }
        WriteLines(lines, file);
    }
}

void ConversionLog::RemoveAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& file : Files()) {
        std::error_code error;
        std::filesystem::remove(file, error);
    }
}

} // namespace iroha
