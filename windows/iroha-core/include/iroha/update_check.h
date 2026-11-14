#pragma once
// アップデートの確認（GitHub Releases の最新版）。移植元: macos/Sources/iroha/UpdateChecker.swift。
// ここは OS に依存しない部分（返事の読み取り・版の比べ方・配布ファイルの選び方・確認した記録）だけ。
// 取得・知らせの窓・インストーラーの起動は iroha-settings の updater.cpp、1 日 1 回の起動は変換サーバ
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace iroha {

struct ReleaseAsset {
    std::string name;
    std::string url;     // browser_download_url
    uint64_t size = 0;
    std::string sha256;  // GitHub の digest（"sha256:…"）の 16 進。無ければ空
};

struct ReleaseInfo {
    std::string tag;       // "v0.18.0"
    std::string page;      // html_url（リリースノート）
    std::u32string notes;  // body
    std::vector<ReleaseAsset> assets;
};

namespace Update {

// 確認先（releases/latest はプレリリースと下書きを返さない）
constexpr char kLatestReleaseUrl[] = "https://api.github.com/repos/TetsuakiBaba/iroha/releases/latest";
// 自動の確認の間隔（macOS版と同じ 24 時間）
constexpr int64_t kCheckIntervalSeconds = 24 * 60 * 60;

// GitHub の Release の JSON を読む（tag_name が無ければ偽）
bool ParseRelease(std::string_view json, ReleaseInfo* release);

// remote が local より新しいか（"v" は無視。プレリリースの識別子も比べる: 0.4.0-beta.1 < 0.4.0-beta.2 < 0.4.0。
// macOS版 UpdateChecker.isNewer と同じ）。local は ComparableVersion を通してから渡す
bool IsNewer(const std::string& remote, const std::string& local);

// 手元の版を比べられる形にする。開発版の git describe の後ろ（"-29-gcf75edd"・"-dirty"）は外して、
// そのタグの版として扱う（外さないと 0.17.1 のプレリリースに見え、0.17.1 を新しい版として知らせてしまう）
std::string ComparableVersion(const std::string& version);

// Windows のインストーラー（iroha-<版>-windows-x64-setup.exe）。無ければ nullptr。
// macOS版のアップデータは「iroha- で始まり .zip で終わる」ものを取るので、Windows の配布物は zip にしない
const ReleaseAsset* WindowsInstaller(const ReleaseInfo& release);

// 確認した記録（<データフォルダ>/update.json）。設定画面が書き、変換サーバは読むだけ
struct State {
    int64_t lastCheck = 0;     // 最後に確認した時刻（UNIX 時刻の秒）
    std::string skipped;       // 「この版をスキップ」した tag
    std::string lastNotified;  // 自動の確認で知らせた tag（同じ版を何度も知らせない）
};
std::filesystem::path StatePath(const std::filesystem::path& dataDirectory);
State LoadState(const std::filesystem::path& path);
bool SaveState(const std::filesystem::path& path, const State& state);

// 自動の確認の時期か（前の確認から 24 時間たった。時計が戻っていたら確認する）
bool IsDue(const State& state, int64_t now);

} // namespace Update
} // namespace iroha
