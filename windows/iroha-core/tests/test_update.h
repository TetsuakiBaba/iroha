#pragma once
// アップデートの確認（版の比べ方・GitHub の返事の読み取り・配布ファイルの選び方・確認した記録）のテスト。
// 版の比べ方は macOS版 UpdateChecker.isNewer と同じ規則。
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include "iroha/update_check.h"

namespace {

void TestUpdateIsNewer() {
    using iroha::Update::IsNewer;
    EXPECT_TRUE(IsNewer("v0.18.0", "0.17.1"));
    EXPECT_TRUE(IsNewer("v0.17.10", "0.17.9"));
    EXPECT_TRUE(IsNewer("v1.0.0", "0.99.99"));
    EXPECT_TRUE(!IsNewer("v0.17.1", "0.17.1"));
    EXPECT_TRUE(!IsNewer("v0.17.0", "0.17.1"));
    // プレリリース: 0.4.0-beta.1 < 0.4.0-beta.2 < 0.4.0
    EXPECT_TRUE(IsNewer("v0.4.0-beta.2", "0.4.0-beta.1"));
    EXPECT_TRUE(IsNewer("v0.4.0", "0.4.0-beta.2"));
    EXPECT_TRUE(!IsNewer("v0.4.0-beta.2", "0.4.0"));
    EXPECT_TRUE(IsNewer("v0.4.0-beta.10", "0.4.0-beta.9"));
}

// 開発版の git describe の後ろは外し、そのタグの版として比べる
void TestUpdateComparableVersion() {
    using iroha::Update::ComparableVersion;
    EXPECT_TRUE(ComparableVersion("0.17.1-29-gcf75edd-dirty") == "0.17.1");
    EXPECT_TRUE(ComparableVersion("0.17.1-29-gcf75edd") == "0.17.1");
    EXPECT_TRUE(ComparableVersion("0.17.1-dirty") == "0.17.1");
    EXPECT_TRUE(ComparableVersion("0.18.0-beta.1-3-gabc1234") == "0.18.0-beta.1");
    EXPECT_TRUE(ComparableVersion("0.18.0-beta.1") == "0.18.0-beta.1");
    EXPECT_TRUE(ComparableVersion("0.18.0") == "0.18.0");
    EXPECT_TRUE(!iroha::Update::IsNewer("v0.17.1", ComparableVersion("0.17.1-29-gcf75edd-dirty")));
    EXPECT_TRUE(iroha::Update::IsNewer("v0.18.0", ComparableVersion("0.17.1-29-gcf75edd-dirty")));
}

void TestUpdateParseRelease() {
    const std::string text =
        R"({"tag_name":"v0.18.0","html_url":"https://github.com/TetsuakiBaba/iroha/releases/tag/v0.18.0",)"
        R"("body":"変更点","assets":[)"
        R"({"name":"iroha-0.18.0.zip","browser_download_url":"https://example.com/a.zip","size":10},)"
        R"({"name":"iroha-0.18.0-linux-x86_64.tar.gz","browser_download_url":"https://example.com/b","size":11},)"
        R"({"name":"iroha-0.18.0-windows-x64-setup.exe","browser_download_url":"https://example.com/c.exe",)"
        R"("size":12345,"digest":"sha256:ABCDEF0123"}]})";
    iroha::ReleaseInfo release;
    EXPECT_TRUE(iroha::Update::ParseRelease(text, &release));
    EXPECT_TRUE(release.tag == "v0.18.0");
    EXPECT_EQ(release.notes, U"変更点");
    EXPECT_TRUE(release.assets.size() == 3);
    const iroha::ReleaseAsset* installer = iroha::Update::WindowsInstaller(release);
    EXPECT_TRUE(installer && installer->name == "iroha-0.18.0-windows-x64-setup.exe");
    EXPECT_TRUE(installer && installer->size == 12345 && installer->sha256 == "abcdef0123");
    // macOS版の zip・Linux版だけなら Windows のインストーラーは無い
    release.assets.pop_back();
    EXPECT_TRUE(iroha::Update::WindowsInstaller(release) == nullptr);
    EXPECT_TRUE(!iroha::Update::ParseRelease(R"({"message":"Not Found"})", &release));
    EXPECT_TRUE(!iroha::Update::ParseRelease("not json", &release));
}

void TestUpdateState() {
    const int64_t now = 1800000000;
    iroha::Update::State state;
    EXPECT_TRUE(iroha::Update::IsDue(state, now)); // まだ確認したことがない
    state.lastCheck = now;
    EXPECT_TRUE(!iroha::Update::IsDue(state, now + 3600));
    EXPECT_TRUE(iroha::Update::IsDue(state, now + iroha::Update::kCheckIntervalSeconds));
    EXPECT_TRUE(iroha::Update::IsDue(state, now - 60)); // 時計が戻った

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("iroha-update-" + iroha::GenerateUuid()) / "update.json";
    state.skipped = "v0.18.0";
    state.lastNotified = "v0.18.1";
    EXPECT_TRUE(iroha::Update::SaveState(path, state));
    const iroha::Update::State loaded = iroha::Update::LoadState(path);
    EXPECT_TRUE(loaded.lastCheck == now && loaded.skipped == "v0.18.0" && loaded.lastNotified == "v0.18.1");
    std::error_code ignored;
    std::filesystem::remove_all(path.parent_path(), ignored);
    EXPECT_TRUE(iroha::Update::LoadState(path).lastCheck == 0);
}

void RunUpdateTests() {
    TestUpdateIsNewer();
    TestUpdateComparableVersion();
    TestUpdateParseRelease();
    TestUpdateState();
}

} // namespace
