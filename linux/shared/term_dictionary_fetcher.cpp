#include "term_dictionary_fetcher.h"

#include <curl/curl.h>

#include <cstdio>
#include <mutex>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

#include "iroha/sha256.h"
#include "iroha/unicode.h"
#include "iroha/user_dictionary.h"

namespace iroha {

namespace {

std::string FileName(const std::string& url) {
    const size_t slash = url.find_last_of('/');
    return slash == std::string::npos ? url : url.substr(slash + 1);
}

size_t WriteToFile(char* data, size_t size, size_t count, void* user) {
    return std::fwrite(data, size, count, static_cast<std::FILE*>(user)) * size;
}

} // namespace

// url を path に落とす（libcurl。https と file:// を扱う。キャッシュは持たない）
bool TermDictionaryFetcher::Download(const std::string& url, const std::filesystem::path& path, std::string* error) {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    std::FILE* file = std::fopen(PathToUtf8(path).c_str(), "wb");
    if (!file) {
        if (error) *error = "書き込めません: " + PathToUtf8(path);
        return false;
    }
    CURL* curl = curl_easy_init();
    CURLcode code = CURLE_FAILED_INIT;
    long status = 0;
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "iroha");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        // 30 秒の間なにも届かなければ打ち切る
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToFile);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
        code = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
    }
    const bool closed = std::fclose(file) == 0;
    if (code != CURLE_OK || !closed) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        if (error) {
            *error = "取得できませんでした（" + std::string(curl_easy_strerror(code)) +
                     (status ? "、HTTP " + std::to_string(status) : std::string()) + "）: " + FileName(url);
        }
        return false;
    }
    return true;
}

namespace {

bool ReadFile(const std::filesystem::path& path, std::string* out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    *out = buffer.str();
    return true;
}

std::filesystem::path TemporaryPath(const std::filesystem::path& directory, const char* stem) {
    return directory / (std::string(stem) + "-" + GenerateUuid() + ".download");
}

} // namespace

std::string TermDictionaryFetcher::Sha256Hex(const std::filesystem::path& path) { return Sha256::FileHex(path); }

std::string TermDictionaryFetcher::NowIso8601() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

bool TermDictionaryFetcher::FetchCatalog(const std::string& url, TermDictionaryCatalog* catalog,
                                         std::string* error) {
    std::error_code temporaryError;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(temporaryError);
    if (temporaryError) {
        if (error) *error = "一時フォルダがありません";
        return false;
    }
    const std::filesystem::path temporary = TemporaryPath(directory, "iroha-term-catalog");
    std::string text;
    const bool downloaded = Download(url, temporary, error) && ReadFile(temporary, &text);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    if (!downloaded) return false;
    return TermDictionaryCatalog::Parse(text, catalog, error);
}

bool TermDictionaryFetcher::Install(const TermDictionaryCatalog::Dictionary& dictionary,
                                    const std::filesystem::path& dataDirectory,
                                    TermDictionaryInstall::Record* record, std::string* error) {
    if (!TermDictionaryInstall::IsValidId(dictionary.id)) {
        if (error) *error = "辞書一覧の ID が正しくありません: " + dictionary.id;
        return false;
    }
    const std::filesystem::path directory = TermDictionaryInstall::Directory(dataDirectory);
    std::error_code fileError;
    std::filesystem::create_directories(directory, fileError);
    const std::filesystem::path temporary = TemporaryPath(directory, dictionary.id.c_str());
    const auto fail = [&](const std::string& message) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        if (error && !message.empty()) *error = message;
        return false;
    };
    if (!Download(dictionary.file.url, temporary, error)) return fail("");

    const uintmax_t size = std::filesystem::file_size(temporary, fileError);
    if (fileError || size != dictionary.file.bytes) {
        return fail("辞書の大きさが違います（期待 " + std::to_string(dictionary.file.bytes) +
                    " バイト、実際 " + (fileError ? std::string("不明") : std::to_string(size)) +
                    " バイト）");
    }
    if (Sha256Hex(temporary) != dictionary.file.sha256) {
        return fail("辞書が壊れています（チェックサム不一致）。もう一度お試しください");
    }
    const std::filesystem::path destination = TermDictionaryInstall::FilePath(dataDirectory, dictionary.id);
    // 同じフォルダの中の名前の付け替え（rename(2)）なので、置き換えの途中の状態は見えない
    std::filesystem::rename(temporary, destination, fileError);
    if (fileError) return fail("辞書を置けませんでした: " + fileError.message());
    const TermDictionaryInstall::Record installed =
        TermDictionaryInstall::Record::From(dictionary, NowIso8601());
    if (!TermDictionaryInstall::WriteRecord(dataDirectory, installed)) {
        if (error) *error = "辞書の記録を書けませんでした";
        return false;
    }
    if (record) *record = installed;
    return true;
}

} // namespace iroha
