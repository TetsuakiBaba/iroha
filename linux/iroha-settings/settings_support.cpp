#include "settings_support.h"

#include <algorithm>
#include <curl/curl.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <tuple>

#include "iroha/benchmark_catalog.h"
#include "iroha/platform.h"
#include "iroha/sha256.h"
#include "iroha/term_dictionary.h"
#include "iroha/typo_normalizer_catalog.h"
#include "iroha/unicode.h"

namespace iroha::settings {

namespace {

struct TabInfo {
    const char* name;
    const char* title;
};
// Tab の並びと同じ順
const TabInfo kTabs[kTabCount] = {
    {"model", "モデル"},          {"input", "入力"},  {"dictionary", "ユーザ辞書"},
    {"terms", "専門辞書"},        {"rules", "変換ルール"}, {"ai", "AI"},
    {"selection", "選択テキスト"}, {"learning", "学習・記録"}, {"about", "情報"},
};

} // namespace

const char* TabName(Tab tab) { return kTabs[static_cast<int>(tab)].name; }
const char* TabTitle(Tab tab) { return kTabs[static_cast<int>(tab)].title; }

std::optional<Tab> ParseTabName(const std::string& name) {
    for (int i = 0; i < kTabCount; ++i) {
        if (name == kTabs[i].name) return static_cast<Tab>(i);
    }
    return std::nullopt;
}

// ---- 置き場所（IBus のエンジン（shared/engine_host.cpp）と同じ環境変数・同じ場所） ----

std::filesystem::path ConfigPath() { return platform::DataDirectory() / "config.json"; }
std::filesystem::path ModelsDirectory() { return platform::DataDirectory() / "models"; }
std::filesystem::path UserDictionaryPath() {
    return platform::StorePath("IROHA_USER_DICT", "user-dictionary.json");
}
std::filesystem::path LearningPath() { return platform::StorePath("IROHA_LEARNING", "learning.json"); }
std::filesystem::path RewriteRulesPath() {
    return platform::StorePath("IROHA_REWRITE_RULES", "user-rewrite-rules.json");
}
std::filesystem::path InputHistoryDirectory() { return platform::StorePath("IROHA_INPUT_HISTORY", "input-history"); }
std::filesystem::path ConversionLogDirectory() {
    return platform::StorePath("IROHA_CONVERSION_LOG", std::filesystem::path("logs") / "conversions");
}

// ---- 変換モデル ----

const std::vector<KnownModel>& KnownModels() {
    static const std::vector<KnownModel> models = {
        {"iroha-kkc-v1（既定・文字単位T5 / 約82MB）", "iroha-kkc-v1-Q5_K_M.gguf",
         "https://github.com/TetsuakiBaba/iroha/releases/download/iroha-kkc-v1/iroha-kkc-v1-Q5_K_M.gguf",
         81535360, "f060e259dbed28754f71e1634bc01a5d9d735d519f8ff72c333cac77c859efda"},
        {"zenz-v3.1-small（以前の既定 95M / 約70MB）", "zenz-v3.1-small-Q5_K_M.gguf",
         "https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf/resolve/main/ggml-model-Q5_K_M.gguf",
         73871968, "4de930c06bef8c263aa1aa40684af206db4ce1b96375b3b8ed0ea508e0b14f6c"},
        {"zenz-v3.1-xsmall（高速・軽量 26M / 約21MB）", "zenz-v3.1-xsmall-Q5_K_M.gguf",
         "https://huggingface.co/Miwa-Keita/zenz-v3.1-xsmall-gguf/resolve/main/ggml-model-Q5_K_M.gguf",
         20970304, "189638370c43292fd54ba5e83854b24887ddd57e8914e19095514e663f60c7f5"},
    };
    return models;
}

std::string CurrentModelFileName(const Config& config) {
    if (config.model.empty()) return PathToUtf8(DefaultModelPath(ModelsDirectory()).filename());
    // 絶対パスで指定してあれば、そのファイル名で一覧と見比べる
    return PathToUtf8(PathFromUtf8(Utf32ToUtf8(config.model)).filename());
}

std::optional<size_t> CurrentModelIndex(const Config& config) {
    const std::string current = CurrentModelFileName(config);
    const auto& models = KnownModels();
    for (size_t i = 0; i < models.size(); ++i) {
        if (current == models[i].fileName) return i;
    }
    return std::nullopt;
}

namespace {

size_t WriteToFile(char* data, size_t size, size_t count, void* user) {
    return std::fwrite(data, size, count, static_cast<std::FILE*>(user)) * size;
}

int OnProgress(void* user, curl_off_t total, curl_off_t received, curl_off_t, curl_off_t) {
    const auto* progress = static_cast<const DownloadProgress*>(user);
    if (!*progress) return 0;
    return (*progress)(static_cast<uint64_t>(received), static_cast<uint64_t>(total)) ? 0 : 1;
}

} // namespace

bool VerifyFile(const std::filesystem::path& path, uint64_t bytes, const std::string& sha256, std::string* error) {
    std::error_code sizeError;
    const uintmax_t size = std::filesystem::file_size(path, sizeError);
    if (sizeError || size != bytes) {
        if (error) {
            *error = "ファイルの大きさが合いません（" + (sizeError ? std::string("読めません") : std::to_string(size)) +
                     " バイト。期待は " + std::to_string(bytes) + " バイト）";
        }
        return false;
    }
    if (Sha256::FileHex(path) != sha256) {
        if (error) *error = "SHA-256 が一致しません（途中で切れたか、配布元のファイルが変わっています）";
        return false;
    }
    return true;
}

bool DownloadModel(const KnownModel& model, const std::filesystem::path& modelsDirectory,
                   const DownloadProgress& progress, std::string* error) {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    std::error_code ignored;
    std::filesystem::create_directories(modelsDirectory, ignored);
    const std::filesystem::path destination = modelsDirectory / model.fileName;
    const std::filesystem::path temporary = TemporaryPath(destination);
    std::FILE* file = std::fopen(PathToUtf8(temporary).c_str(), "wb");
    if (!file) {
        if (error) *error = "書き込めません: " + PathToUtf8(temporary);
        return false;
    }
    CURL* curl = curl_easy_init();
    CURLcode code = CURLE_FAILED_INIT;
    long status = 0;
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, model.url);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "iroha");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        // 30 秒の間なにも届かなければ打ち切る（term_dictionary_fetcher と同じ）
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToFile);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnProgress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        code = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
    }
    const bool closed = std::fclose(file) == 0;
    if (code != CURLE_OK || !closed) {
        std::filesystem::remove(temporary, ignored);
        if (error) {
            if (code == CURLE_ABORTED_BY_CALLBACK) {
                *error = "取得を止めました";
            } else {
                *error = "取得できませんでした（" + std::string(curl_easy_strerror(code)) +
                         (status ? "、HTTP " + std::to_string(status) : std::string()) + "）";
            }
        }
        return false;
    }
    if (!VerifyFile(temporary, model.bytes, model.sha256, error)) {
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    std::error_code renameError;
    std::filesystem::rename(temporary, destination, renameError);
    if (renameError) {
        std::filesystem::remove(temporary, ignored);
        if (error) *error = "置けませんでした: " + PathToUtf8(destination);
        return false;
    }
    return true;
}

// ---- ライセンス ----

std::vector<LicenseGroup> Licenses(const std::filesystem::path& dataDirectory) {
    std::vector<LicenseGroup> groups;
    // iroha 本体と同梱のもの（macOS版の設定 > 情報 の一覧のうち、Linux版で使っているもの）
    LicenseGroup base;
    base.entries = {
        {"iroha", "Tetsuaki Baba", "MIT License", "", "https://github.com/TetsuakiBaba/iroha", ""},
        {"iroha-kkc-v1", "Tetsuaki Baba", "CC BY-SA 4.0", "既定の変換モデル",
         "https://github.com/TetsuakiBaba/iroha/releases/tag/iroha-kkc-v1", ""},
        {"zenz-v2.5-dataset", "Keita Miwa", "CC BY-SA 4.0", "既定の変換モデルの学習元",
         "https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset", ""},
        {"llama.cpp", "ggml-org", "MIT License", "変換モデルの推論エンジン", "https://github.com/ggml-org/llama.cpp", ""},
        {"AzooKeyKanaKanjiConverter", "ensan (azooKey)", "MIT License",
         "候補ウィンドウの辞書ラティス（C++ に書き写したもの）", "https://github.com/azooKey/AzooKeyKanaKanjiConverter", ""},
        {"azooKey_dictionary_storage", "azooKey", "Apache License 2.0", "辞書データ",
         "https://github.com/azooKey/azooKey_dictionary_storage", ""},
        {"Mozc", "Google Inc.", "BSD 3-Clause License", "記号・顔文字・絵文字のデータ", "https://github.com/google/mozc",
         ""},
    };
    // 設定画面から取得できる zenz のモデル（入れてあるときだけ）
    const std::filesystem::path models = dataDirectory / "models";
    std::error_code ignored;
    if (std::filesystem::exists(models / KnownModels()[1].fileName, ignored)) {
        base.entries.push_back({"zenz-v3.1-small", "Keita Miwa", "CC BY-SA 4.0", "変換モデル",
                                "https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf", ""});
    }
    if (std::filesystem::exists(models / KnownModels()[2].fileName, ignored)) {
        base.entries.push_back({"zenz-v3.1-xsmall", "Keita Miwa", "CC BY-SA 4.0", "変換モデル",
                                "https://huggingface.co/Miwa-Keita/zenz-v3.1-xsmall-gguf", ""});
    }
    groups.push_back(std::move(base));

    // 打ち間違い訂正モデルとその学習元（入れたときの記録から。学習元はモデルごとに違う）
    const auto typoRecord = TypoNormalizerInstall::IsInstalled(dataDirectory)
                                ? TypoNormalizerInstall::InstalledRecord(dataDirectory)
                                : std::nullopt;
    if (const auto& record = typoRecord) {
        LicenseGroup typo;
        typo.entries.push_back({"打ち間違い訂正モデル（" + record->id + "）", "Tetsuaki Baba", Utf32ToUtf8(record->license),
                                "", record->page.empty() ? "https://github.com/TetsuakiBaba/iroha" : record->page, ""});
        for (const auto& source : record->sources) {
            typo.entries.push_back({Utf32ToUtf8(source.name), Utf32ToUtf8(source.holder), Utf32ToUtf8(source.license),
                                    "打ち間違い訂正モデルの学習元", source.url, ""});
        }
        if (!record->sources.empty()) {
            typo.footnote = "学習元のデータは、文への分割・読みの付与など加工して学習に使っています。"
                            "データそのものはアプリにもモデルにも含めていません。";
        }
        groups.push_back(std::move(typo));
    }

    // 専門辞書の出典（入れたときの記録から。同じ出典は 1 行にまとめ、使っている辞書の名前を並べる。macOS版と同じ）
    const auto records = TermDictionaryInstall::InstalledRecords(dataDirectory);
    if (!records.empty()) {
        using Key = std::tuple<std::u32string, std::u32string, std::u32string, std::string, std::string>;
        std::vector<Key> order;
        std::map<Key, std::vector<std::u32string>> dictionaries;
        for (const auto& record : records) {
            for (const auto& source : record.sources) {
                const Key key{source.name, source.holder, source.license, source.url, source.licenseUrl};
                auto& names = dictionaries[key];
                if (names.empty()) order.push_back(key);
                names.push_back(record.name);
            }
        }
        LicenseGroup terms;
        for (const Key& key : order) {
            std::string names;
            for (const auto& name : dictionaries[key]) {
                if (!names.empty()) names += "・";
                names += Utf32ToUtf8(name);
            }
            terms.entries.push_back({Utf32ToUtf8(std::get<0>(key)), Utf32ToUtf8(std::get<1>(key)),
                                     Utf32ToUtf8(std::get<2>(key)), "専門辞書（" + names + "）", std::get<3>(key),
                                     std::get<4>(key)});
        }
        if (!terms.entries.empty()) {
            terms.footnote = "専門辞書は、元データから読みをひらがなにそろえ、重複を除くなど加工して作っています。"
                             "加工した辞書も元データと同じライセンスで配布しています。内容の正確さは保証しません。";
            groups.push_back(std::move(terms));
        }
    }

    // 変換のベンチマークの評価セットの出典（入れたものだけ。抜粋と全件は同じ出典なので、出典ごとに 1 行。Windows版と同じ）
    LicenseGroup benchmarks;
    std::vector<std::string> benchmarkSources;
    for (const auto& record : BenchmarkInstall::InstalledRecords(dataDirectory)) {
        for (const auto& source : record.sources) {
            if (std::find(benchmarkSources.begin(), benchmarkSources.end(), source.name) != benchmarkSources.end()) {
                continue;
            }
            benchmarkSources.push_back(source.name);
            benchmarks.entries.push_back({source.name, source.holder, source.license,
                                          "変換のベンチマークの評価セット（" + record.name + "、" + record.license +
                                              "）の出典",
                                          source.url, ""});
        }
    }
    if (!benchmarks.entries.empty()) groups.push_back(std::move(benchmarks));
    return groups;
}

std::string FormatBytes(uint64_t bytes) {
    char text[64];
    if (bytes >= 1024ull * 1024 * 1024) {
        std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024ull * 1024) {
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024));
    } else if (bytes >= 1024) {
        std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(text, sizeof(text), "%llu バイト", static_cast<unsigned long long>(bytes));
    }
    return text;
}

namespace {
const char* const kSelectionTriggers[] = {"off", "bubble", "immediateMenu", "optionMenu"};
}

int SelectionTriggerIndex(const std::string& value) {
    for (int i = 0; i < 4; ++i) {
        if (value == kSelectionTriggers[i]) return i;
    }
    return 1;
}

std::string SelectionTriggerValue(int index) {
    return index >= 0 && index < 4 ? kSelectionTriggers[index] : "bubble";
}

} // namespace iroha::settings
