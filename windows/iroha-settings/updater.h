#pragma once
// アップデートの確認と入れ替え（macOS版 UpdateChecker の Windows版）。
// GitHub の最新の Release を見て、新しい版があれば知らせる。「アップデート」でインストーラー
// （iroha-<版>-windows-x64-setup.exe）を落とし、大きさと SHA-256（GitHub の digest）を照合してから
// 無言の形（/SILENT）で起動する。インストーラーが変換サーバと設定画面を止めて入れ替え、変換サーバを起動し直す。
//
// 自動の確認は変換サーバが 1 日 1 回 `iroha-settings.exe --check-update` で呼ぶ（窓を出すのはこちらの役目。
// 変換サーバに窓の部品を持たせないため）。手動の確認は 情報 > アップデートを確認

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <string>

#include "iroha/config.h"

namespace updater {

enum class Result {
    None,      // 知らせることが無かった・後で・スキップ
    Launched,  // インストーラーを起動した（呼び出し側はすぐ終わる）
};

// manual: 手動（結果を必ず知らせる・24 時間の間隔とスキップを見ない）か自動か
Result CheckAndOffer(HWND owner, const iroha::Config& config, const std::filesystem::path& dataDirectory,
                     const std::string& currentVersion, bool manual);

} // namespace updater
