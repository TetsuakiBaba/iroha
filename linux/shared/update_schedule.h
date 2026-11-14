#pragma once
// アップデートの自動の確認の起動（macOS版 UpdateChecker.autoCheckIfDue・Windows版 iroha-server の StartUpdateChecks）。
// IBus・fcitx5 のエンジンが持つ。起動から 2 分後とその後 1 時間ごとに確認の時期か（設定の autoUpdateCheck が ON で、
// 前の確認から 24 時間たった）を見て、時期なら設定画面を `--check-update` で起動する。GitHub に問い合わせて
// 新しい版を知らせ、入れ替えるのは設定画面（エンジンに窓の部品を持たせないため）。設定画面は確認した時刻を
// <データフォルダ>/update.json に書くので、ここは読むだけ
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace iroha {

class UpdateSchedule {
public:
    // settingsPath: 設定画面の実行ファイル。log: 起動したことの記録（無くてよい）
    UpdateSchedule(std::filesystem::path settingsPath, std::function<void(const std::string&)> log);
    ~UpdateSchedule();
    UpdateSchedule(const UpdateSchedule&) = delete;
    UpdateSchedule& operator=(const UpdateSchedule&) = delete;

    // 時期なら設定画面を起動するか（裏のスレッドが 1 時間ごとに呼ぶ。単体テスト用に公開）。
    // 設定画面が記録を書けなかったときに起動し続けないよう、起動は 1 時間に 1 回まで（lastLaunch）
    static bool ShouldLaunch(bool autoUpdateCheck, int64_t lastCheck, int64_t lastLaunch, int64_t now);

private:
    void Run();

    std::filesystem::path settingsPath_;
    std::function<void(const std::string&)> log_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace iroha
