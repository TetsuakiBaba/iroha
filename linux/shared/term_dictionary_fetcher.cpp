#include "term_dictionary_fetcher.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

#include "iroha/sha256.h"
#include "iroha/unicode.h"
#include "iroha/user_dictionary.h"

namespace iroha {

namespace {

extern "C" char** environ;

// コマンドを引数の配列のまま起動して終わるのを待つ（シェルを通さない）。終了コードを返す（起動できなければ -1）
int Run(const std::vector<std::string>& arguments) {
    std::vector<char*> argv;
    for (const std::string& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string FileName(const std::string& url) {
    const size_t slash = url.find_last_of('/');
    return slash == std::string::npos ? url : url.substr(slash + 1);
}

} // namespace

// url を path に落とす（curl、無ければ wget。キャッシュは持たない）
bool TermDictionaryFetcher::Download(const std::string& url, const std::filesystem::path& path, std::string* error) {
    const std::string output = PathToUtf8(path);
    int status = Run({"curl", "--fail", "--silent", "--show-error", "--location", "--proto", "=https,http,file",
                      "--output", output, "--", url});
    if (status == -1) status = Run({"wget", "--quiet", "--output-document", output, "--", url});
    if (status != 0) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        if (error) {
            *error = status == -1 ? "curl も wget も見つかりません"
                                  : "取得できませんでした（終了コード " + std::to_string(status) + "）: " + FileName(url);
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
