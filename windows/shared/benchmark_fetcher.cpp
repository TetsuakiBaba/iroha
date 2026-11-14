#include "benchmark_fetcher.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fstream>
#include <sstream>

#include "iroha/user_dictionary.h"
#include "term_dictionary_fetcher.h"

namespace iroha {

namespace {

std::filesystem::path TemporaryPath(const std::filesystem::path& directory, const std::string& stem) {
    return directory / (stem + "-" + GenerateUuid() + ".download");
}

std::string Lowercased(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

} // namespace

bool BenchmarkFetcher::FetchCatalog(const std::string& url, BenchmarkCatalog* catalog, std::string* error) {
    wchar_t directory[MAX_PATH];
    if (GetTempPathW(MAX_PATH, directory) == 0) {
        if (error) *error = "一時フォルダがありません";
        return false;
    }
    const std::filesystem::path temporary = TemporaryPath(directory, "iroha-benchmark-catalog");
    std::string text;
    bool downloaded = TermDictionaryFetcher::Download(url, temporary, error);
    if (downloaded) {
        std::ifstream file(temporary, std::ios::binary);
        std::stringstream buffer;
        buffer << file.rdbuf();
        text = buffer.str();
        downloaded = static_cast<bool>(file);
    }
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    if (!downloaded) return false;
    return BenchmarkCatalog::Parse(text, catalog, error);
}

bool BenchmarkFetcher::Install(const BenchmarkCatalog::Entry& entry, const std::filesystem::path& dataDirectory,
                               BenchmarkInstall::Record* record, std::string* error) {
    if (!BenchmarkInstall::IsValidId(entry.id)) {
        if (error) *error = "評価セットの一覧の ID が正しくありません: " + entry.id;
        return false;
    }
    const std::filesystem::path directory = BenchmarkInstall::Directory(dataDirectory);
    std::error_code fileError;
    std::filesystem::create_directories(directory, fileError);
    const std::filesystem::path temporary = TemporaryPath(directory, entry.id);
    const auto fail = [&](const std::string& message) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        if (error && !message.empty()) *error = message;
        return false;
    };
    if (!TermDictionaryFetcher::Download(entry.file.url, temporary, error)) return fail("");

    const uintmax_t size = std::filesystem::file_size(temporary, fileError);
    if (fileError || size != entry.file.bytes) {
        return fail("評価セットの大きさが違います（期待 " + std::to_string(entry.file.bytes) + " バイト、実際 " +
                    (fileError ? std::string("不明") : std::to_string(size)) + " バイト）");
    }
    if (TermDictionaryFetcher::Sha256Hex(temporary) != Lowercased(entry.file.sha256)) {
        return fail("評価セットが壊れています（チェックサム不一致）。もう一度お試しください");
    }
    const std::filesystem::path destination = BenchmarkInstall::FilePath(dataDirectory, entry.id);
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return fail("評価セットを置けませんでした（" + std::to_string(GetLastError()) + "）");
    }
    const BenchmarkInstall::Record installed =
        BenchmarkInstall::Record::From(entry, TermDictionaryFetcher::NowIso8601());
    if (!BenchmarkInstall::WriteRecord(dataDirectory, installed)) {
        if (error) *error = "評価セットの記録を書けませんでした";
        return false;
    }
    if (record) *record = installed;
    return true;
}

} // namespace iroha
