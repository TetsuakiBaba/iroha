#pragma once
#include <filesystem>
#include <string>

namespace iroha {

// アプリ設定（%LOCALAPPDATA%\iroha\config.json）。
// 設定GUIが書き、変換サーバが起動時とReload要求時に読む。
struct Config {
    // 使用するモデル。空なら既定（zenz-v3.1-small-Q5_K_M.gguf）。
    // ファイル名なら models ディレクトリ相対、絶対パスならそのまま使う
    std::u32string model;
};

Config LoadConfig(const std::filesystem::path& path);
bool SaveConfig(const std::filesystem::path& path, const Config& config);

} // namespace iroha
