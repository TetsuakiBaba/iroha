#include "term_dictionary_fetcher.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bcrypt.h>
#include <urlmon.h>
#include <wininet.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "iroha/unicode.h"
#include "iroha/user_dictionary.h"

namespace iroha {

namespace {

std::wstring Wide(const std::string& utf8) { return Utf32ToUtf16(Utf8ToUtf32(utf8)); }

std::string FileName(const std::string& url) {
    const size_t slash = url.find_last_of('/');
    return slash == std::string::npos ? url : url.substr(slash + 1);
}

// url を path に落とす。前に落とした内容（Internet Explorer のキャッシュ）は使わない
bool Download(const std::string& url, const std::filesystem::path& path, std::string* error) {
    const std::wstring wideUrl = Wide(url);
    DeleteUrlCacheEntryW(wideUrl.c_str());
    const HRESULT hr = URLDownloadToFileW(nullptr, wideUrl.c_str(), path.c_str(), 0, nullptr);
    if (FAILED(hr)) {
        char message[64];
        std::snprintf(message, sizeof(message), "（0x%08X）", static_cast<unsigned>(hr));
        if (error) *error = "取得できませんでした" + std::string(message) + ": " + FileName(url);
        return false;
    }
    return true;
}

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

std::string TermDictionaryFetcher::Sha256Hex(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buffer(1 << 16);
        bool ok = true;
        while (ok && file) {
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize got = file.gcount();
            if (got > 0) {
                ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                    static_cast<ULONG>(got), 0) == 0;
            }
        }
        unsigned char digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            static const char kDigits[] = "0123456789abcdef";
            for (unsigned char byte : digest) {
                hex += kDigits[byte >> 4];
                hex += kDigits[byte & 0xF];
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return hex;
}

std::string TermDictionaryFetcher::NowIso8601() {
    SYSTEMTIME now = {};
    GetSystemTime(&now);
    char text[32];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02uZ", now.wYear, now.wMonth, now.wDay,
                  now.wHour, now.wMinute, now.wSecond);
    return text;
}

bool TermDictionaryFetcher::FetchCatalog(const std::string& url, TermDictionaryCatalog* catalog,
                                         std::string* error) {
    wchar_t directory[MAX_PATH];
    if (GetTempPathW(MAX_PATH, directory) == 0) {
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
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return fail("辞書を置けませんでした（" + std::to_string(GetLastError()) + "）");
    }
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
