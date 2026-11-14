#include "iroha/benchmark_catalog.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "iroha/json.h"
#include "iroha/term_dictionary.h"
#include "iroha/unicode.h"

namespace iroha {

namespace {

using json::Value;

std::optional<std::string> StringOf(const Value& object, const char* key) {
    const Value* value = object.Find(key);
    if (!value || value->type != Value::Type::String) return std::nullopt;
    return value->string;
}

bool ReadSources(const Value& object, std::vector<BenchmarkCatalog::Source>* sources) {
    const Value* list = object.Find("sources");
    if (!list || list->type != Value::Type::Array) return false;
    for (const Value& entry : list->array) {
        const auto name = StringOf(entry, "name");
        const auto holder = StringOf(entry, "holder");
        const auto license = StringOf(entry, "license");
        const auto url = StringOf(entry, "url");
        if (!name || !holder || !license || !url) return false;
        sources->push_back({*name, *holder, *license, *url});
    }
    return true;
}

Value SourcesValue(const std::vector<BenchmarkCatalog::Source>& sources) {
    Value list = json::MakeArray();
    for (const auto& source : sources) {
        Value entry = json::MakeObject();
        entry.Set("name", json::MakeString(source.name));
        entry.Set("holder", json::MakeString(source.holder));
        entry.Set("license", json::MakeString(source.license));
        entry.Set("url", json::MakeString(source.url));
        list.array.push_back(std::move(entry));
    }
    return list;
}

bool ReadFile(const std::filesystem::path& path, std::string* out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    *out = buffer.str();
    return true;
}

std::string Lowercased(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return text;
}

} // namespace

std::string BenchmarkCatalog::DefaultUrl() {
#ifdef _WIN32
    wchar_t* env = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&env, &length, L"IROHA_BENCHMARK_CATALOG") == 0 && env != nullptr) {
        const std::string value = Utf32ToUtf8(Utf16ToUtf32(env));
        std::free(env);
        if (!value.empty()) return value;
    }
#else
    if (const char* env = std::getenv("IROHA_BENCHMARK_CATALOG")) {
        if (env[0] != '\0') return env;
    }
#endif
    return "https://raw.githubusercontent.com/TetsuakiBaba/iroha/main/models/benchmarks.json";
}

bool BenchmarkCatalog::Parse(const std::string& text, BenchmarkCatalog* catalog, std::string* error) {
    Value root;
    const auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (!json::Parse(text, &root) || root.type != Value::Type::Object) return fail("評価セットの一覧が読めません");
    const Value* version = root.Find("formatVersion");
    const Value* list = root.Find("benchmarks");
    if (!version || version->type != Value::Type::Number || !list || list->type != Value::Type::Array) {
        return fail("評価セットの一覧が読めません");
    }
    BenchmarkCatalog parsed;
    parsed.formatVersion = static_cast<int>(version->number);
    if (parsed.formatVersion > kSupportedFormatVersion) {
        return fail("評価セットの一覧の形式が新しすぎます（v" + std::to_string(parsed.formatVersion) +
                    "）。irohaを更新してください");
    }
    for (const Value& item : list->array) {
        Entry entry;
        const auto id = StringOf(item, "id");
        const auto name = StringOf(item, "name");
        const auto license = StringOf(item, "license");
        const Value* items = item.Find("items");
        const Value* file = item.Find("file");
        if (!id || !name || !license || !items || items->type != Value::Type::Number || !file) {
            return fail("評価セットの一覧が読めません");
        }
        const auto url = StringOf(*file, "url");
        const auto sha256 = StringOf(*file, "sha256");
        const Value* bytes = file->Find("bytes");
        if (!url || !sha256 || !bytes || bytes->type != Value::Type::Number) {
            return fail("評価セットの一覧が読めません");
        }
        entry.id = *id;
        entry.name = *name;
        entry.summary = StringOf(item, "summary").value_or("");
        entry.items = static_cast<int>(items->number);
        entry.file.url = *url;
        entry.file.bytes = static_cast<uint64_t>(bytes->number);
        entry.file.sha256 = *sha256;
        entry.license = *license;
        if (!ReadSources(item, &entry.sources)) return fail("評価セットの一覧が読めません");
        entry.notice = StringOf(item, "notice");
        parsed.benchmarks.push_back(std::move(entry));
    }
    *catalog = std::move(parsed);
    return true;
}

const BenchmarkCatalog::Entry* BenchmarkCatalog::Find(const std::string& id) const {
    for (const Entry& entry : benchmarks) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

// ---- 入れた評価セット ----

BenchmarkInstall::Record BenchmarkInstall::Record::From(const BenchmarkCatalog::Entry& entry,
                                                        const std::string& installedAt) {
    Record record;
    record.id = entry.id;
    record.name = entry.name;
    record.items = entry.items;
    record.sha256 = Lowercased(entry.file.sha256);
    record.license = entry.license;
    record.sources = entry.sources;
    record.notice = entry.notice;
    record.installedAt = installedAt;
    return record;
}

std::filesystem::path BenchmarkInstall::Directory(const std::filesystem::path& dataDirectory) {
    return dataDirectory / "benchmarks";
}

std::filesystem::path BenchmarkInstall::FilePath(const std::filesystem::path& dataDirectory, const std::string& id) {
    return Directory(dataDirectory) / (id + ".jsonl");
}

std::filesystem::path BenchmarkInstall::RecordPath(const std::filesystem::path& dataDirectory, const std::string& id) {
    return Directory(dataDirectory) / (id + ".json");
}

bool BenchmarkInstall::IsValidId(const std::string& id) { return TermDictionaryInstall::IsValidId(id); }

std::string BenchmarkInstall::EncodeRecord(const Record& record) {
    Value root = json::MakeObject();
    root.Set("id", json::MakeString(record.id));
    root.Set("name", json::MakeString(record.name));
    root.Set("items", json::MakeNumber(record.items));
    root.Set("sha256", json::MakeString(record.sha256));
    root.Set("license", json::MakeString(record.license));
    root.Set("sources", SourcesValue(record.sources));
    if (record.notice) root.Set("notice", json::MakeString(*record.notice));
    root.Set("installedAt", json::MakeString(record.installedAt));
    return json::Serialize(root, true);
}

std::optional<BenchmarkInstall::Record> BenchmarkInstall::DecodeRecord(const std::string& text) {
    Value root;
    if (!json::Parse(text, &root) || root.type != Value::Type::Object) return std::nullopt;
    Record record;
    const auto id = StringOf(root, "id");
    const auto name = StringOf(root, "name");
    const auto sha256 = StringOf(root, "sha256");
    const auto license = StringOf(root, "license");
    const auto installedAt = StringOf(root, "installedAt");
    const Value* items = root.Find("items");
    if (!id || !name || !sha256 || !license || !installedAt || !items || items->type != Value::Type::Number ||
        !ReadSources(root, &record.sources)) {
        return std::nullopt;
    }
    record.id = *id;
    record.name = *name;
    record.items = static_cast<int>(items->number);
    record.sha256 = *sha256;
    record.license = *license;
    record.notice = StringOf(root, "notice");
    record.installedAt = *installedAt;
    return record;
}

std::vector<BenchmarkInstall::Record> BenchmarkInstall::InstalledRecords(const std::filesystem::path& dataDirectory) {
    std::vector<Record> records;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(Directory(dataDirectory), error)) {
        const std::filesystem::path& path = entry.path();
        if (path.extension() != ".json") continue;
        const std::string id = path.stem().u8string();
        std::string text;
        if (!IsValidId(id) || !std::filesystem::is_regular_file(FilePath(dataDirectory, id), error) ||
            !ReadFile(path, &text)) {
            continue;
        }
        std::optional<Record> record = DecodeRecord(text);
        if (record && record->id == id) records.push_back(std::move(*record));
    }
    std::sort(records.begin(), records.end(), [](const Record& a, const Record& b) { return a.id < b.id; });
    return records;
}

bool BenchmarkInstall::WriteRecord(const std::filesystem::path& dataDirectory, const Record& record) {
    std::error_code error;
    std::filesystem::create_directories(Directory(dataDirectory), error);
    const std::filesystem::path path = RecordPath(dataDirectory, record.id);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        const std::string text = EncodeRecord(record);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) return false;
    }
    std::filesystem::rename(temporary, path, error);
    return !error;
}

bool BenchmarkInstall::Remove(const std::filesystem::path& dataDirectory, const std::string& id) {
    if (!IsValidId(id)) return false;
    std::error_code error;
    // 素性を先に消す（途中で失敗しても「入っている」とは見なされない）
    std::filesystem::remove(RecordPath(dataDirectory, id), error);
    if (error) return false;
    std::filesystem::remove(FilePath(dataDirectory, id), error);
    return !error;
}

bool BenchmarkInstall::IsOutdated(const Record& record, const BenchmarkCatalog::Entry& entry) {
    return Lowercased(entry.file.sha256) != record.sha256;
}

} // namespace iroha
