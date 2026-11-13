#include "typo_normalizer_fetcher.h"

#include <fstream>
#include <sstream>

#include "iroha/unicode.h"
#include "iroha/user_dictionary.h"
#include "term_dictionary_fetcher.h"

namespace iroha {

bool TypoNormalizerFetcher::FetchCatalog(const std::string& url, TypoNormalizerCatalog* catalog, std::string* error) {
    std::error_code temporaryError;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(temporaryError);
    if (temporaryError) {
        if (error) *error = "一時フォルダがありません";
        return false;
    }
    const std::filesystem::path temporary = directory / ("iroha-typo-catalog-" + GenerateUuid() + ".download");
    bool downloaded = TermDictionaryFetcher::Download(url, temporary, error);
    std::string text;
    if (downloaded) {
        std::ifstream file(temporary, std::ios::binary);
        std::stringstream buffer;
        buffer << file.rdbuf();
        text = buffer.str();
        downloaded = static_cast<bool>(file) || !text.empty();
    }
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    if (!downloaded) return false;
    return TypoNormalizerCatalog::Parse(text, catalog, error);
}

bool TypoNormalizerFetcher::Install(const TypoNormalizerCatalog::Model& model, const std::u32string& license,
                                    const std::filesystem::path& dataDirectory, TypoNormalizerInstall::Record* record,
                                    std::string* error) {
    const std::filesystem::path destination = TypoNormalizerInstall::Directory(dataDirectory);
    // 入れ替えを同じドライブの中の名前の付け替えで済ませるため、一時フォルダは設置先の隣に作る
    const std::filesystem::path staging =
        destination.parent_path() / ("typo-normalizer-" + GenerateUuid() + ".download");
    std::error_code fileError;
    std::filesystem::create_directories(staging, fileError);
    const auto fail = [&](const std::string& message) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        if (error && !message.empty()) *error = message;
        return false;
    };
    if (fileError) return fail("一時フォルダを作れません");

    const struct {
        const TypoNormalizerCatalog::File* file;
        const char* name;
    } files[] = {{&model.manifest, "manifest.json"}, {&model.weights, "weights.bin"}};
    for (const auto& item : files) {
        const std::filesystem::path path = staging / item.name;
        if (!TermDictionaryFetcher::Download(item.file->url, path, error)) return fail("");
        const uintmax_t size = std::filesystem::file_size(path, fileError);
        if (fileError || size != item.file->bytes) {
            return fail(std::string(item.name) + " の大きさが違います（期待 " + std::to_string(item.file->bytes) +
                        " バイト、実際 " + (fileError ? std::string("不明") : std::to_string(size)) + " バイト）");
        }
        if (TermDictionaryFetcher::Sha256Hex(path) != item.file->sha256) {
            return fail(std::string(item.name) + " が壊れています（チェックサム不一致）。もう一度お試しください");
        }
    }

    // 照合に通ったので入れ替える。古いものを消してから移すので、中途半端な組み合わせが残らない
    std::filesystem::remove_all(destination, fileError);
    std::filesystem::rename(staging, destination, fileError);
    if (fileError) return fail("モデルを置けませんでした: " + fileError.message());
    TypoNormalizerInstall::Record installed;
    installed.id = model.id;
    installed.name = model.name;
    installed.sha256 = model.weights.sha256;
    installed.installedAt = TermDictionaryFetcher::NowIso8601();
    installed.license = license;
    installed.sources = model.sources;
    installed.page = model.page;
    if (!TypoNormalizerInstall::WriteRecord(dataDirectory, installed)) {
        if (error) *error = "モデルの記録を書けませんでした";
        return false;
    }
    if (record) *record = installed;
    return true;
}

} // namespace iroha
