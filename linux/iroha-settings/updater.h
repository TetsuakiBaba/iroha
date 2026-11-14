#pragma once
// アップデートの確認と入れ替え（macOS版 UpdateChecker・Windows版 windows/iroha-settings/updater の Linux版）。
// GitHub の最新の Release を見て、新しい版があれば知らせる。「アップデート」で配布物
// （iroha-<版>-linux-x86_64.tar.gz）を落とし、大きさと SHA-256（GitHub の digest）を照合してから展開し、
// 中の install.sh を 2 回に分けて実行する:
//   pkexec install.sh --system   管理者の部分（/usr/local/lib/iroha などへ置く。管理者のパスワードを求める）
//   install.sh --session         利用者の部分（古いエンジンを止めて ibus-daemon を起動し直す）
// 入れ替えの手順は新しい版の install.sh が決める（配布物の形が変わっても、ここを変えずに済むように）。
//
// 自動の確認は IBus・fcitx5 のエンジンが 1 日 1 回 `iroha-settings --check-update` で呼ぶ（linux/shared/update_schedule）。
// 手動の確認は 設定 > 情報 > アップデート の「今すぐ確認」と、入力メニューの「アップデートを確認…」
// （`iroha-settings --check-update-now`）
#include <gtk/gtk.h>

#include <filesystem>
#include <functional>
#include <string>

namespace updater {

struct Host {
    GtkApplication* application = nullptr;
    // 知らせの窓を載せる窓（設定の窓が無ければ nullptr を返す）
    std::function<GtkWindow*()> parent;
    // 入れ替えたあと、設定の窓が開いていれば呼ぶ（設定画面を新しい版で開き直す）
    std::function<void()> relaunch;
};

// manual: 手動（結果を必ず知らせる・24 時間の間隔とスキップを見ない）か自動か。
// 確認・取得は裏のスレッドで行い、すぐに戻る。確認している間に呼ばれたら、手動なら今の窓を前に出すだけ
void CheckAndOffer(const Host& host, const std::filesystem::path& dataDirectory, const std::string& currentVersion,
                   bool manual);

} // namespace updater
