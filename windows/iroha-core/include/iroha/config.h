#pragma once
#include <filesystem>
#include <string>

namespace iroha {

// 既定のかな漢字変換モデル（macOS版の DataDirectory.defaultModelFileName と同じ。
// 文字単位 T5 の iroha-kkc-v1、Q5_K_M。差し替えるときは windows/scripts/fetch-model.ps1 と
// iroha-settings のモデル一覧も合わせる）
inline constexpr char kDefaultModelFileName[] = "iroha-kkc-v1-Q5_K_M.gguf";
// 2026-11-13 までの Windows 版の既定
inline constexpr char kPreviousDefaultModelFileName[] = "zenz-v3.1-small-Q5_K_M.gguf";

// models フォルダの中の既定のモデルのパス。既定のモデルをまだ取得していない環境で、
// 以前の既定のモデルがあればそちらを返す（取得するまで変換できなくなるのを避ける）
std::filesystem::path DefaultModelPath(const std::filesystem::path& modelsDirectory);

// アプリ設定（%LOCALAPPDATA%\iroha\config.json）。
// 設定GUIが書き、変換サーバが起動時とReload要求時に読む。
struct Config {
    // 使用するモデル。空なら既定（DefaultModelPath）。
    // ファイル名なら models ディレクトリ相対、絶対パスならそのまま使う
    std::u32string model;
    // ライブ変換（入力中に自動で変換して見せる）を使うか。偽なら Space を押すまでかなのまま
    bool liveConversion = true;
    // 文節の候補ウィンドウに記号・顔文字・絵文字（Mozc のデータ）を足すか
    bool symbolCandidates = true;
};

Config LoadConfig(const std::filesystem::path& path);
bool SaveConfig(const std::filesystem::path& path, const Config& config);

} // namespace iroha
