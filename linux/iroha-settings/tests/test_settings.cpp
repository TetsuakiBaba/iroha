// 設定画面の、画面に依存しない部分のテスト（settings_support）。
// データフォルダは一時フォルダに替える（IROHA_DATA_DIR）。取得は file:// の URL で確かめる（通信しない）

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include "iroha/config.h"
#include "iroha/sha256.h"
#include "iroha/term_dictionary.h"
#include "iroha/typo_normalizer_catalog.h"
#include "iroha/unicode.h"
#include "settings_support.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::fprintf(stderr, "失敗 (%d 行目): %s\n", line, expression);
}
#define CHECK(expression) Check((expression), #expression, __LINE__)

void WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << text;
}

bool HasEntry(const std::vector<iroha::settings::LicenseGroup>& groups, const std::string& name) {
    for (const auto& group : groups) {
        for (const auto& entry : group.entries) {
            if (entry.name == name) return true;
        }
    }
    return false;
}

void TestTabs() {
    using iroha::settings::Tab;
    // --tab の名前は Windows版と同じ（about は Linux版で足した）
    const char* const names[] = {"model", "input", "dictionary", "terms", "rules",
                                 "ai",    "selection", "learning", "about"};
    for (int i = 0; i < iroha::settings::kTabCount; ++i) {
        CHECK(std::string(iroha::settings::TabName(static_cast<Tab>(i))) == names[i]);
        CHECK(iroha::settings::ParseTabName(names[i]) == static_cast<Tab>(i));
    }
    CHECK(!iroha::settings::ParseTabName("voice"));
    CHECK(!iroha::settings::ParseTabName(""));
    CHECK(std::string(iroha::settings::TabTitle(Tab::Learning)) == "学習・記録");
}

void TestModels(const std::filesystem::path& data) {
    const auto& models = iroha::settings::KnownModels();
    CHECK(models.size() == 3);
    // 既定のモデルは config.h の既定と同じ
    CHECK(std::string(models[0].fileName) == iroha::kDefaultModelFileName);
    for (const auto& model : models) CHECK(std::string(model.sha256).size() == 64);

    iroha::Config config;
    CHECK(iroha::settings::CurrentModelFileName(config) == iroha::kDefaultModelFileName);
    CHECK(iroha::settings::CurrentModelIndex(config) == size_t{0});
    // 既定のモデルが無く、以前の既定のモデルがあればそちら（IBus のエンジンと同じ決め方）
    WriteFile(data / "models" / iroha::kPreviousDefaultModelFileName, "x");
    CHECK(iroha::settings::CurrentModelFileName(config) == iroha::kPreviousDefaultModelFileName);
    CHECK(iroha::settings::CurrentModelIndex(config) == size_t{1});
    config.model = U"zenz-v3.1-xsmall-Q5_K_M.gguf";
    CHECK(iroha::settings::CurrentModelIndex(config) == size_t{2});
    config.model = U"/opt/models/zenz-v3.1-xsmall-Q5_K_M.gguf";
    CHECK(iroha::settings::CurrentModelIndex(config) == size_t{2});
    config.model = U"other.gguf";
    CHECK(!iroha::settings::CurrentModelIndex(config));
}

void TestDownload(const std::filesystem::path& work) {
    const std::filesystem::path source = work / "source" / "model.gguf";
    const std::string body(200000, 'g');
    WriteFile(source, body);
    const std::string sha = iroha::Sha256::FileHex(source);
    const std::string url = "file://" + iroha::PathToUtf8(source);
    const std::filesystem::path models = work / "download-models";

    // 照合できれば置き、一時ファイルを残さない
    iroha::settings::KnownModel good{"テスト", "good.gguf", url.c_str(), body.size(), sha.c_str()};
    uint64_t lastReceived = 0;
    std::string error;
    CHECK(iroha::settings::DownloadModel(
        good, models,
        [&](uint64_t received, uint64_t) {
            lastReceived = received;
            return true;
        },
        &error));
    CHECK(error.empty());
    CHECK(std::filesystem::file_size(models / "good.gguf") == body.size());
    CHECK(!std::filesystem::exists(iroha::TemporaryPath(models / "good.gguf")));
    CHECK(lastReceived == body.size());

    // 大きさが違えば置かない
    iroha::settings::KnownModel wrongSize{"テスト", "size.gguf", url.c_str(), body.size() + 1, sha.c_str()};
    error.clear();
    CHECK(!iroha::settings::DownloadModel(wrongSize, models, nullptr, &error));
    CHECK(!error.empty());
    CHECK(!std::filesystem::exists(models / "size.gguf"));
    CHECK(!std::filesystem::exists(iroha::TemporaryPath(models / "size.gguf")));

    // SHA-256 が違えば置かない
    const std::string otherSha(64, '0');
    iroha::settings::KnownModel wrongSha{"テスト", "sha.gguf", url.c_str(), body.size(), otherSha.c_str()};
    error.clear();
    CHECK(!iroha::settings::DownloadModel(wrongSha, models, nullptr, &error));
    CHECK(error.find("SHA-256") != std::string::npos);
    CHECK(!std::filesystem::exists(models / "sha.gguf"));

    // 取れなければ置かない
    const std::string missing = "file://" + iroha::PathToUtf8(work / "source" / "missing.gguf");
    iroha::settings::KnownModel absent{"テスト", "absent.gguf", missing.c_str(), body.size(), sha.c_str()};
    error.clear();
    CHECK(!iroha::settings::DownloadModel(absent, models, nullptr, &error));
    CHECK(!error.empty());
    CHECK(!std::filesystem::exists(models / "absent.gguf"));
    CHECK(!std::filesystem::exists(iroha::TemporaryPath(models / "absent.gguf")));

    // 進み具合の関数が偽を返したら止める
    iroha::settings::KnownModel stopped{"テスト", "stopped.gguf", url.c_str(), body.size(), sha.c_str()};
    error.clear();
    CHECK(!iroha::settings::DownloadModel(
        stopped, models, [](uint64_t, uint64_t) { return false; }, &error));
    CHECK(error == "取得を止めました");
    CHECK(!std::filesystem::exists(models / "stopped.gguf"));
}

void TestLicenses(const std::filesystem::path& data) {
    // 何も入れていなければ本体と同梱のものだけ（以前の既定の zenz-v3.1-small は TestModels で置いた）
    auto groups = iroha::settings::Licenses(data);
    CHECK(groups.size() == 1);
    CHECK(HasEntry(groups, "iroha"));
    CHECK(HasEntry(groups, "iroha-kkc-v1"));
    CHECK(HasEntry(groups, "Mozc"));
    CHECK(HasEntry(groups, "zenz-v3.1-small"));
    CHECK(!HasEntry(groups, "zenz-v3.1-xsmall"));

    // 打ち間違い訂正モデル: 記録だけでモデルの本体が無ければ出さない
    iroha::TypoNormalizerInstall::Record typo;
    typo.id = "typo-test";
    typo.name = U"テストのモデル";
    typo.sha256 = std::string(64, 'a');
    typo.license = U"CC BY-SA 4.0";
    typo.page = "https://example.com/typo";
    typo.sources.push_back({U"学習元A", U"作者A", U"CC BY 4.0", "https://example.com/a"});
    CHECK(iroha::TypoNormalizerInstall::WriteRecord(data, typo));
    CHECK(iroha::settings::Licenses(data).size() == 1);
    const auto typoDirectory = iroha::TypoNormalizerInstall::Directory(data);
    WriteFile(typoDirectory / "manifest.json", "{}");
    WriteFile(typoDirectory / "weights.bin", "w");
    groups = iroha::settings::Licenses(data);
    CHECK(groups.size() == 2);
    CHECK(HasEntry(groups, "打ち間違い訂正モデル（typo-test）"));
    CHECK(HasEntry(groups, "学習元A"));
    CHECK(groups.size() >= 2 && !groups[1].footnote.empty());

    // 専門辞書: 同じ出典は 1 行にまとめ、辞書の名前を並べる
    const iroha::TermDictionaryCatalog::Source shared{U"共通の出典", U"作者B", U"CC0", "https://example.com/b",
                                                      "https://example.com/b/license"};
    for (const auto& [id, name] : {std::pair<std::string, std::u32string>{"term-a", U"医学"},
                                   std::pair<std::string, std::u32string>{"term-b", U"工学"}}) {
        iroha::TermDictionaryInstall::Record record;
        record.id = id;
        record.name = name;
        record.license = U"CC0";
        record.sha256 = std::string(64, 'b');
        record.version = "20261114";
        record.installedAt = "2026-11-14T00:00:00Z";
        record.sources.push_back(shared);
        CHECK(iroha::TermDictionaryInstall::WriteRecord(data, record));
        WriteFile(iroha::TermDictionaryInstall::FilePath(data, id), "いがく\t医学\n");
    }
    groups = iroha::settings::Licenses(data);
    CHECK(groups.size() == 3);
    if (groups.size() < 3) return;
    CHECK(groups[2].entries.size() == 1);
    if (groups[2].entries.empty()) return;
    CHECK(groups[2].entries[0].note == "専門辞書（医学・工学）");
    CHECK(groups[2].entries[0].licenseUrl == "https://example.com/b/license");
    CHECK(!groups[2].footnote.empty());
}

void TestFormatBytes() {
    CHECK(iroha::settings::FormatBytes(512) == "512 バイト");
    CHECK(iroha::settings::FormatBytes(2048) == "2.0 KB");
    CHECK(iroha::settings::FormatBytes(81535360) == "77.8 MB");
}

} // namespace

int main() {
    std::filesystem::path work = std::filesystem::temp_directory_path() /
                                 ("iroha-settings-tests-" + std::to_string(std::rand()) + "-" +
                                  std::to_string(static_cast<long>(std::time(nullptr))));
    std::filesystem::remove_all(work);
    const std::filesystem::path data = work / "data";
    std::filesystem::create_directories(data);
    setenv("IROHA_DATA_DIR", iroha::PathToUtf8(data).c_str(), 1);

    TestTabs();
    TestModels(data);
    TestDownload(work);
    TestLicenses(data);
    TestFormatBytes();

    std::filesystem::remove_all(work);
    std::printf("%d 件中 %d 件失敗\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
