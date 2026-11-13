#include "iroha/typo_normalizer_catalog.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "iroha/json.h"
#include "iroha/typo_normalizer.h"
#include "iroha/unicode.h"

namespace iroha {

namespace {

std::string String8(const json::Value* value) { return value ? value->AsString() : ""; }
std::u32string String32(const json::Value* value) { return Utf8ToUtf32(String8(value)); }

bool Number(const json::Value* value, uint64_t* out) {
    if (!value || value->type != json::Value::Type::Number || value->number < 0) return false;
    *out = static_cast<uint64_t>(value->number);
    return true;
}

bool ParseFile(const json::Value* value, TypoNormalizerCatalog::File* file) {
    if (!value || value->type != json::Value::Type::Object) return false;
    file->url = String8(value->Find("url"));
    file->sha256 = String8(value->Find("sha256"));
    for (char& c : file->sha256) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    return !file->url.empty() && file->sha256.size() == 64 && Number(value->Find("bytes"), &file->bytes);
}

std::vector<TypoNormalizerCatalog::Source> ParseSources(const json::Value* value) {
    std::vector<TypoNormalizerCatalog::Source> sources;
    if (!value || value->type != json::Value::Type::Array) return sources;
    for (const json::Value& item : value->array) {
        sources.push_back({String32(item.Find("name")), String32(item.Find("holder")), String32(item.Find("license")),
                           String8(item.Find("url"))});
    }
    return sources;
}

std::string Quote(const std::u32string& text) { return json::QuoteString(Utf32ToUtf8(text)); }

} // namespace

std::string TypoNormalizerCatalog::DefaultUrl() {
#ifdef _WIN32
    wchar_t* env = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&env, &length, L"IROHA_TYPO_CATALOG") == 0 && env != nullptr) {
        const std::string value = Utf32ToUtf8(Utf16ToUtf32(env));
        std::free(env);
        if (!value.empty()) return value;
    }
#else
    if (const char* env = std::getenv("IROHA_TYPO_CATALOG")) {
        if (env[0] != '\0') return env;
    }
#endif
    return "https://raw.githubusercontent.com/TetsuakiBaba/iroha/main/models/typo-normalizer-format2.json";
}

bool TypoNormalizerCatalog::Parse(const std::string& text, TypoNormalizerCatalog* catalog, std::string* error) {
    json::Value root;
    if (!json::Parse(text, &root) || root.type != json::Value::Type::Object) {
        if (error) *error = "モデルの一覧を読めません";
        return false;
    }
    uint64_t format = 0;
    if (!Number(root.Find("formatVersion"), &format)) {
        if (error) *error = "モデルの一覧に形式の番号がありません";
        return false;
    }
    if (format > kSupportedFormatVersion) {
        if (error) *error = "モデルの一覧の形式が新しすぎます（v" + std::to_string(format) + "）。irohaを更新してください";
        return false;
    }
    TypoNormalizerCatalog result;
    result.formatVersion = static_cast<int>(format);
    result.license = String32(root.Find("license"));
    result.attribution = String32(root.Find("attribution"));
    if (const json::Value* list = root.Find("models"); list && list->type == json::Value::Type::Array) {
        for (const json::Value& item : list->array) {
            Model model;
            model.id = String8(item.Find("id"));
            model.name = String32(item.Find("name"));
            model.summary = String32(item.Find("summary"));
            model.license = String32(item.Find("license"));
            model.attribution = String32(item.Find("attribution"));
            model.sources = ParseSources(item.Find("sources"));
            model.page = String8(item.Find("page"));
            if (model.id.empty() || !ParseFile(item.Find("manifest"), &model.manifest) ||
                !ParseFile(item.Find("weights"), &model.weights)) {
                continue;
            }
            result.models.push_back(std::move(model));
        }
    }
    if (result.models.empty()) {
        if (error) *error = "配布中のモデルが見つかりません";
        return false;
    }
    *catalog = std::move(result);
    return true;
}

const TypoNormalizerCatalog::Model* TypoNormalizerCatalog::Find(const std::string& id) const {
    for (const Model& model : models) {
        if (model.id == id) return &model;
    }
    return nullptr;
}

// ---- 設置 ----

std::filesystem::path TypoNormalizerInstall::Directory(const std::filesystem::path& dataDirectory) {
    return dataDirectory / "models" / "typo-normalizer";
}

std::filesystem::path TypoNormalizerInstall::RecordPath(const std::filesystem::path& dataDirectory) {
    return Directory(dataDirectory) / "installed.json";
}

bool TypoNormalizerInstall::IsInstalled(const std::filesystem::path& dataDirectory) {
    return TypoNormalizer::IsModelDirectory(Directory(dataDirectory));
}

std::optional<TypoNormalizerInstall::Record> TypoNormalizerInstall::InstalledRecord(
    const std::filesystem::path& dataDirectory) {
    std::ifstream file(RecordPath(dataDirectory), std::ios::binary);
    if (!file) return std::nullopt;
    std::stringstream buffer;
    buffer << file.rdbuf();
    return DecodeRecord(buffer.str());
}

bool TypoNormalizerInstall::WriteRecord(const std::filesystem::path& dataDirectory, const Record& record) {
    std::error_code error;
    std::filesystem::create_directories(Directory(dataDirectory), error);
    const std::filesystem::path path = RecordPath(dataDirectory);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file << EncodeRecord(record);
        if (!file) return false;
    }
    std::filesystem::rename(temporary, path, error);
    return !error;
}

bool TypoNormalizerInstall::Remove(const std::filesystem::path& dataDirectory) {
    std::error_code error;
    std::filesystem::remove_all(Directory(dataDirectory), error);
    return !error;
}

bool TypoNormalizerInstall::NeedsUpdate(const std::filesystem::path& dataDirectory,
                                        const TypoNormalizerCatalog::Model& model) {
    if (!IsInstalled(dataDirectory)) return true;
    const auto record = InstalledRecord(dataDirectory);
    if (!record) return true;
    std::string installed = record->sha256;
    for (char& c : installed) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    return record->id != model.id || installed != model.weights.sha256;
}

// macOS版の JSONEncoder（キーは名前順・無い項目は書かない）と同じキー
std::string TypoNormalizerInstall::EncodeRecord(const Record& record) {
    std::string out = "{\"id\":" + json::QuoteString(record.id) +
                      ",\"installedAt\":" + json::QuoteString(record.installedAt);
    if (!record.license.empty()) out += ",\"license\":" + Quote(record.license);
    out += ",\"name\":" + Quote(record.name);
    if (!record.page.empty()) out += ",\"page\":" + json::QuoteString(record.page);
    out += ",\"sha256\":" + json::QuoteString(record.sha256);
    if (!record.sources.empty()) {
        out += ",\"sources\":[";
        for (size_t i = 0; i < record.sources.size(); ++i) {
            const auto& source = record.sources[i];
            if (i > 0) out += ",";
            out += "{\"holder\":" + Quote(source.holder) + ",\"license\":" + Quote(source.license) +
                   ",\"name\":" + Quote(source.name) + ",\"url\":" + json::QuoteString(source.url) + "}";
        }
        out += "]";
    }
    return out + "}";
}

std::optional<TypoNormalizerInstall::Record> TypoNormalizerInstall::DecodeRecord(const std::string& text) {
    json::Value root;
    if (!json::Parse(text, &root) || root.type != json::Value::Type::Object) return std::nullopt;
    Record record;
    record.id = String8(root.Find("id"));
    record.name = String32(root.Find("name"));
    record.sha256 = String8(root.Find("sha256"));
    record.installedAt = String8(root.Find("installedAt"));
    record.license = String32(root.Find("license"));
    record.sources = ParseSources(root.Find("sources"));
    record.page = String8(root.Find("page"));
    if (record.id.empty() || record.sha256.empty()) return std::nullopt;
    return record;
}

} // namespace iroha
