#include "iroha/term_dictionary.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "iroha/json.h"
#include "iroha/unicode.h"
#include "iroha/user_dictionary.h"

namespace iroha {

namespace {

bool ReadFile(const std::filesystem::path& path, std::string* out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    *out = buffer.str();
    return true;
}

// 書いてから名前を変える（途中で止まっても半端なファイルを残さない）
bool WriteFileAtomically(const std::filesystem::path& path, const std::string& content) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!file) return false;
    }
    std::filesystem::rename(temporary, path, error);
    if (error) std::filesystem::remove(temporary, error);
    return !error;
}

std::u32string String32(const json::Value* value) {
    return value ? Utf8ToUtf32(value->AsString()) : std::u32string();
}

std::string String8(const json::Value* value) { return value ? value->AsString() : std::string(); }

bool Number(const json::Value* value, uint64_t* out) {
    if (!value || value->type != json::Value::Type::Number || value->number < 0) return false;
    *out = static_cast<uint64_t>(value->number);
    return true;
}

std::string Lowercase(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

std::vector<TermDictionaryCatalog::Source> ParseSources(const json::Value* value) {
    std::vector<TermDictionaryCatalog::Source> sources;
    if (!value || value->type != json::Value::Type::Array) return sources;
    for (const json::Value& item : value->array) {
        TermDictionaryCatalog::Source source;
        source.name = String32(item.Find("name"));
        source.holder = String32(item.Find("holder"));
        source.license = String32(item.Find("license"));
        source.url = String8(item.Find("url"));
        source.licenseUrl = String8(item.Find("licenseURL"));
        sources.push_back(std::move(source));
    }
    return sources;
}

std::string EncodeSources(const std::vector<TermDictionaryCatalog::Source>& sources) {
    std::string out = "[";
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto& source = sources[i];
        if (i > 0) out += ",";
        // macOS版の JSONEncoder（sortedKeys）と同じくキーの順に並べる
        out += "{\"holder\":" + json::QuoteString(Utf32ToUtf8(source.holder)) +
               ",\"license\":" + json::QuoteString(Utf32ToUtf8(source.license));
        if (!source.licenseUrl.empty()) out += ",\"licenseURL\":" + json::QuoteString(source.licenseUrl);
        out += ",\"name\":" + json::QuoteString(Utf32ToUtf8(source.name)) +
               ",\"url\":" + json::QuoteString(source.url) + "}";
    }
    return out + "]";
}

} // namespace

// ---- 種類 ----

std::optional<TermKind> ParseTermKind(const std::string& value) {
    if (value == "noun") return TermKind::kNoun;
    if (value == "person") return TermKind::kPerson;
    if (value == "place") return TermKind::kPlace;
    if (value == "properNoun") return TermKind::kProperNoun;
    return std::nullopt;
}

const char* TermKindName(TermKind kind) {
    switch (kind) {
    case TermKind::kPerson: return "person";
    case TermKind::kPlace: return "place";
    case TermKind::kProperNoun: return "properNoun";
    case TermKind::kNoun: break;
    }
    return "noun";
}

// ---- ファイル ----

std::vector<TermPair> TermDictionary::Parse(const std::string& utf8) {
    std::vector<TermPair> pairs;
    size_t start = 0;
    while (start < utf8.size()) {
        size_t end = utf8.find('\n', start);
        if (end == std::string::npos) end = utf8.size();
        std::string_view line(utf8.data() + start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        start = end + 1;
        if (line.empty() || line.front() == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string_view::npos) continue;
        const std::u32string reading = Utf8ToUtf32(line.substr(0, tab));
        const std::u32string word = Utf8ToUtf32(line.substr(tab + 1));
        if (!UserDictionary::IsImportableReading(reading) || word.empty() ||
            word.find(U'\t') != std::u32string::npos) {
            continue;
        }
        pairs.push_back({reading, word});
    }
    return pairs;
}

// ---- カタログ ----

std::string TermDictionaryCatalog::DefaultUrl() {
#ifdef _WIN32
    wchar_t* env = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&env, &length, L"IROHA_TERM_DICTIONARY_CATALOG") == 0 && env != nullptr) {
        const std::string value = Utf32ToUtf8(Utf16ToUtf32(env));
        std::free(env);
        if (!value.empty()) return value;
    }
#else
    if (const char* env = std::getenv("IROHA_TERM_DICTIONARY_CATALOG")) {
        if (env[0] != '\0') return env;
    }
#endif
    return "https://raw.githubusercontent.com/TetsuakiBaba/iroha/main/models/term-dictionaries.json";
}

bool TermDictionaryCatalog::Parse(const std::string& text, TermDictionaryCatalog* catalog,
                                  std::string* error) {
    json::Value root;
    if (!json::Parse(text, &root) || root.type != json::Value::Type::Object) {
        if (error) *error = "辞書一覧を読めません";
        return false;
    }
    uint64_t format = 0;
    if (!Number(root.Find("formatVersion"), &format)) {
        if (error) *error = "辞書一覧に形式の番号がありません";
        return false;
    }
    if (format > kSupportedFormatVersion) {
        if (error) {
            *error = "辞書一覧の形式が新しすぎます（v" + std::to_string(format) +
                     "）。irohaを更新してください";
        }
        return false;
    }
    TermDictionaryCatalog result;
    result.formatVersion = static_cast<int>(format);
    const json::Value* list = root.Find("dictionaries");
    if (!list || list->type != json::Value::Type::Array) {
        if (error) *error = "辞書一覧に辞書がありません";
        return false;
    }
    for (const json::Value& item : list->array) {
        Dictionary dictionary;
        dictionary.id = String8(item.Find("id"));
        dictionary.name = String32(item.Find("name"));
        dictionary.group = String32(item.Find("group"));
        dictionary.summary = String32(item.Find("summary"));
        dictionary.license = String32(item.Find("license"));
        dictionary.version = String8(item.Find("version"));
        dictionary.sources = ParseSources(item.Find("sources"));
        if (const json::Value* kind = item.Find("kind")) {
            dictionary.kind = ParseTermKind(kind->AsString()).value_or(TermKind::kNoun);
        }
        if (const json::Value* samples = item.Find("samples");
            samples && samples->type == json::Value::Type::Array) {
            for (const json::Value& sample : samples->array) {
                dictionary.samples.push_back(
                    {String32(sample.Find("word")), String32(sample.Find("reading"))});
            }
        }
        const json::Value* file = item.Find("file");
        if (dictionary.id.empty() || !Number(item.Find("entries"), &dictionary.entries) || !file ||
            !Number(file->Find("bytes"), &dictionary.file.bytes)) {
            if (error) *error = "辞書一覧の項目が欠けています";
            return false;
        }
        dictionary.file.url = String8(file->Find("url"));
        dictionary.file.sha256 = Lowercase(String8(file->Find("sha256")));
        result.dictionaries.push_back(std::move(dictionary));
    }
    *catalog = std::move(result);
    return true;
}

const TermDictionaryCatalog::Dictionary* TermDictionaryCatalog::Find(const std::string& id) const {
    for (const Dictionary& dictionary : dictionaries) {
        if (dictionary.id == id) return &dictionary;
    }
    return nullptr;
}

// ---- 設置 ----

TermDictionaryInstall::Record TermDictionaryInstall::Record::From(
    const TermDictionaryCatalog::Dictionary& dictionary, const std::string& installedAt) {
    Record record;
    record.id = dictionary.id;
    record.name = dictionary.name;
    record.group = dictionary.group;
    record.version = dictionary.version;
    record.sha256 = Lowercase(dictionary.file.sha256);
    record.entries = dictionary.entries;
    record.kind = dictionary.kind;
    record.license = dictionary.license;
    record.sources = dictionary.sources;
    record.installedAt = installedAt;
    return record;
}

std::filesystem::path TermDictionaryInstall::Directory(const std::filesystem::path& dataDirectory) {
    return dataDirectory / "dictionaries";
}

std::filesystem::path TermDictionaryInstall::FilePath(const std::filesystem::path& dataDirectory,
                                                      const std::string& id) {
    return Directory(dataDirectory) / (id + ".tsv");
}

std::filesystem::path TermDictionaryInstall::RecordPath(const std::filesystem::path& dataDirectory,
                                                        const std::string& id) {
    return Directory(dataDirectory) / (id + ".json");
}

bool TermDictionaryInstall::IsValidId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

std::string TermDictionaryInstall::EncodeRecord(const Record& record) {
    // macOS版の JSONEncoder（sortedKeys）と同じくキーの順に並べる
    return "{\"entries\":" + std::to_string(record.entries) +
           ",\"group\":" + json::QuoteString(Utf32ToUtf8(record.group)) +
           ",\"id\":" + json::QuoteString(record.id) +
           ",\"installedAt\":" + json::QuoteString(record.installedAt) +
           ",\"kind\":" + json::QuoteString(TermKindName(record.kind)) +
           ",\"license\":" + json::QuoteString(Utf32ToUtf8(record.license)) +
           ",\"name\":" + json::QuoteString(Utf32ToUtf8(record.name)) +
           ",\"sha256\":" + json::QuoteString(record.sha256) +
           ",\"sources\":" + EncodeSources(record.sources) +
           ",\"version\":" + json::QuoteString(record.version) + "}\n";
}

std::optional<TermDictionaryInstall::Record> TermDictionaryInstall::DecodeRecord(
    const std::string& text) {
    json::Value root;
    if (!json::Parse(text, &root) || root.type != json::Value::Type::Object) return std::nullopt;
    Record record;
    record.id = String8(root.Find("id"));
    record.name = String32(root.Find("name"));
    record.group = String32(root.Find("group"));
    record.version = String8(root.Find("version"));
    record.sha256 = Lowercase(String8(root.Find("sha256")));
    record.license = String32(root.Find("license"));
    record.installedAt = String8(root.Find("installedAt"));
    record.sources = ParseSources(root.Find("sources"));
    const json::Value* kind = root.Find("kind");
    const std::optional<TermKind> parsedKind = kind ? ParseTermKind(kind->AsString()) : std::nullopt;
    if (record.id.empty() || record.sha256.empty() || !parsedKind ||
        !Number(root.Find("entries"), &record.entries)) {
        return std::nullopt;
    }
    record.kind = *parsedKind;
    return record;
}

std::vector<TermDictionaryInstall::Record> TermDictionaryInstall::InstalledRecords(
    const std::filesystem::path& dataDirectory) {
    std::vector<Record> records;
    std::error_code error;
    const std::filesystem::path directory = Directory(dataDirectory);
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end;
         it.increment(error)) {
        const std::filesystem::path path = it->path();
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
    std::sort(records.begin(), records.end(),
              [](const Record& a, const Record& b) { return a.id < b.id; });
    return records;
}

bool TermDictionaryInstall::WriteRecord(const std::filesystem::path& dataDirectory,
                                        const Record& record) {
    if (!IsValidId(record.id)) return false;
    return WriteFileAtomically(RecordPath(dataDirectory, record.id), EncodeRecord(record));
}

bool TermDictionaryInstall::Remove(const std::filesystem::path& dataDirectory, const std::string& id) {
    if (!IsValidId(id)) return false;
    std::error_code error;
    for (const std::filesystem::path& path : {RecordPath(dataDirectory, id), FilePath(dataDirectory, id)}) {
        if (std::filesystem::exists(path, error) && !std::filesystem::remove(path, error)) return false;
    }
    return true;
}

bool TermDictionaryInstall::NeedsUpdate(const Record& record,
                                        const TermDictionaryCatalog::Dictionary& dictionary) {
    return record.sha256 != Lowercase(dictionary.file.sha256);
}

std::vector<TermDictionaryPack> TermDictionaryInstall::LoadPacks(
    const std::filesystem::path& dataDirectory, const std::vector<Record>& records) {
    std::vector<TermDictionaryPack> packs;
    for (const Record& record : records) {
        std::string text;
        if (!ReadFile(FilePath(dataDirectory, record.id), &text)) continue;
        packs.push_back({record.kind, TermDictionary::Parse(text)});
    }
    return packs;
}

std::string TermDictionaryInstall::Signature(const std::vector<Record>& records) {
    std::string signature;
    for (const Record& record : records) {
        if (!signature.empty()) signature += ",";
        signature += record.id + ":" + record.sha256;
    }
    return signature;
}

// ---- 辞書名の印 ----

TermDictionaryLookup::TermDictionaryLookup(std::vector<Table> tables) {
    for (Table& table : tables) tables_.push_back(std::make_shared<const Table>(std::move(table)));
}

TermDictionaryLookup TermDictionaryLookup::Load(
    const std::filesystem::path& dataDirectory,
    const std::vector<TermDictionaryInstall::Record>& records) {
    std::vector<Table> tables;
    for (const auto& record : records) {
        Table table;
        if (!ReadFile(TermDictionaryInstall::FilePath(dataDirectory, record.id), &table.data)) continue;
        table.label = Label(record.name);
        table.start = BodyStart(table.data);
        table.id = record.id;
        tables.push_back(std::move(table));
    }
    return TermDictionaryLookup(std::move(tables));
}

TermDictionaryLookup TermDictionaryLookup::LoadFiles(const std::vector<std::filesystem::path>& files) {
    std::vector<Table> tables;
    for (const auto& path : files) {
        Table table;
        if (!ReadFile(path, &table.data)) continue;
        table.label = Utf16ToUtf32(path.stem().wstring());
        table.start = BodyStart(table.data);
        tables.push_back(std::move(table));
    }
    return TermDictionaryLookup(std::move(tables));
}

TermDictionaryLookup TermDictionaryLookup::Select(const std::set<std::string>& ids) const {
    TermDictionaryLookup selected;
    for (const auto& table : tables_) {
        if (!table->id.empty() && ids.count(table->id) > 0) selected.tables_.push_back(table);
    }
    return selected;
}

// ---- 専門辞書の設定 ----

std::set<std::string> TermDictionarySettings::Ids(const std::string& text) {
    std::set<std::string> ids;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(',', start);
        if (end == std::string::npos) end = text.size();
        if (end > start) ids.insert(text.substr(start, end - start));
        start = end + 1;
    }
    return ids;
}

std::string TermDictionarySettings::Text(const std::set<std::string>& ids) {
    std::string text;
    for (const std::string& id : ids) {
        if (id.empty()) continue;
        if (!text.empty()) text += ",";
        text += id;
    }
    return text;
}

std::u32string TermDictionaryLookup::Label(const std::u32string& name) {
    const size_t open = name.find_first_of(U"（(");
    if (open == std::u32string::npos || open == 0) return name;
    return name.substr(0, open);
}

size_t TermDictionaryLookup::BodyStart(const std::string& data) {
    size_t position = 0;
    while (position < data.size() && data[position] == '#') {
        while (position < data.size() && data[position] != '\n') ++position;
        ++position;
    }
    return std::min(position, data.size());
}

bool TermDictionaryLookup::Contains(const std::string& key, const Table& table) {
    const std::string& bytes = table.data;
    size_t low = table.start;
    size_t high = bytes.size();
    while (low < high) {
        // 真ん中を含む行の頭と終わり
        size_t lineStart = (low + high) / 2;
        while (lineStart > low && bytes[lineStart - 1] != '\n') --lineStart;
        size_t lineEnd = lineStart;
        while (lineEnd < high && bytes[lineEnd] != '\n') ++lineEnd;
        // 行と鍵をバイト列として比べる（符号なしで比べる。UTF-8 のバイト順）
        const int order = std::string_view(bytes.data() + lineStart, lineEnd - lineStart)
                              .compare(std::string_view(key));
        if (order == 0) return true;
        if (order < 0) {
            low = lineEnd + 1;
        } else {
            high = lineStart;
        }
    }
    return false;
}

size_t TermDictionaryLookup::LowerBound(const std::string& key, const Table& table) {
    const std::string& bytes = table.data;
    size_t low = table.start;
    size_t high = bytes.size();
    while (low < high) {
        size_t lineStart = (low + high) / 2;
        while (lineStart > low && bytes[lineStart - 1] != '\n') --lineStart;
        size_t lineEnd = lineStart;
        while (lineEnd < high && bytes[lineEnd] != '\n') ++lineEnd;
        if (std::string_view(bytes.data() + lineStart, lineEnd - lineStart).compare(std::string_view(key)) < 0) {
            low = lineEnd + 1;
        } else {
            high = lineStart;
        }
    }
    return low;
}

namespace {

bool StartsWith(const std::string& bytes, size_t position, const std::string& prefix) {
    return position <= bytes.size() && bytes.size() - position >= prefix.size() &&
           bytes.compare(position, prefix.size(), prefix) == 0;
}

} // namespace

bool TermDictionaryLookup::HasLine(const std::string& prefix, const Table& table) {
    return StartsWith(table.data, LowerBound(prefix, table), prefix);
}

std::vector<std::u32string> TermDictionaryLookup::Words(const std::string& key, const Table& table) {
    const std::string& bytes = table.data;
    std::vector<std::u32string> words;
    size_t position = LowerBound(key, table);
    while (StartsWith(bytes, position, key)) {
        size_t lineEnd = position + key.size();
        while (lineEnd < bytes.size() && bytes[lineEnd] != '\n') ++lineEnd;
        std::string_view word(bytes.data() + position + key.size(), lineEnd - position - key.size());
        if (!word.empty() && word.back() == '\r') word.remove_suffix(1);
        if (!word.empty()) words.push_back(Utf8ToUtf32(word));
        position = lineEnd + 1;
    }
    return words;
}

UserDictionary TermDictionaryLookup::LiveDictionary(const std::u32string& reading) const {
    if (tables_.empty()) return UserDictionary();
    const std::vector<std::u32string> characters = SplitGraphemes(reading);
    std::vector<UserDictionaryEntry> entries;
    std::set<std::u32string> seen;
    for (size_t start = 0; start < characters.size(); ++start) {
        std::u32string part;
        for (size_t end = start; end < characters.size(); ++end) {
            part += characters[end];
            const std::string prefix = Utf32ToUtf8(part);
            if (std::none_of(tables_.begin(), tables_.end(),
                             [&](const auto& table) { return HasLine(prefix, *table); })) {
                break;
            }
            if (end - start + 1 < TermDictionary::kMinimumReadingLength || !seen.insert(part).second) continue;
            const std::string key = prefix + "\t";
            std::vector<std::u32string> words;
            for (const auto& table : tables_) {
                for (std::u32string& word : Words(key, *table)) {
                    if (std::find(words.begin(), words.end(), word) == words.end()) words.push_back(std::move(word));
                }
            }
            if (words.size() == 1 && words[0] != part &&
                UserDictionary::IsSuitableForLiveConversion(part, words[0])) {
                entries.push_back(MakeUserDictionaryEntry(part, words[0]));
            }
        }
    }
    return entries.empty() ? UserDictionary() : UserDictionary(std::move(entries));
}

std::vector<std::u32string> TermDictionaryLookup::Labels(const std::u32string& reading,
                                                         const std::u32string& candidate) const {
    if (tables_.empty()) return {};
    const std::vector<std::u32string> readingCharacters = SplitGraphemes(reading);
    const std::vector<std::u32string> candidateCharacters = SplitGraphemes(candidate);
    for (size_t length = readingCharacters.size(); length >= TermDictionary::kMinimumReadingLength;
         --length) {
        // 読みの残り（助詞などのかな）が候補の後ろにそのまま付いているか
        const size_t tailCount = readingCharacters.size() - length;
        if (candidateCharacters.size() <= tailCount) continue;
        bool suffixMatches = true;
        for (size_t i = 0; i < tailCount && suffixMatches; ++i) {
            suffixMatches = candidateCharacters[candidateCharacters.size() - tailCount + i] ==
                            readingCharacters[length + i];
        }
        if (!suffixMatches) continue;
        std::u32string word;
        for (size_t i = 0; i + tailCount < candidateCharacters.size(); ++i) word += candidateCharacters[i];
        std::u32string termReading;
        for (size_t i = 0; i < length; ++i) termReading += readingCharacters[i];
        if (word == termReading) continue;
        const std::string key = Utf32ToUtf8(termReading) + "\t" + Utf32ToUtf8(word);
        std::vector<std::u32string> found;
        for (const auto& table : tables_) {
            if (Contains(key, *table)) found.push_back(table->label);
        }
        if (!found.empty()) return found;
    }
    return {};
}

} // namespace iroha
