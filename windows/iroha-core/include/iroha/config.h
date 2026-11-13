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
    // 句読点（「,」「.」キーで入れる文字の組。"、。" か "，．"。Ctrl+. で切り替えると保存する）
    std::u32string punctuation = U"、。";
    // 入力していないときの Space を全角スペースにするか（偽なら半角。Shift+Space は常に半角）
    bool fullWidthSpace = true;
    // 文節の候補として求める数（3〜16）
    int candidateCount = 8;
    // 予測変換（入力履歴から読みの先頭が一致する語を出す。入力履歴もこれが ON の間だけ記録する。既定 OFF）
    bool predictive = false;
    // プライベートモード（学習・入力履歴・予測・ユーザ辞書を止める。Ctrl+Alt+P で切り替えると保存する）
    bool privateMode = false;
    // 変換記録（確定を logs/conversions/ に残す。左文脈に文書の文章が入るので既定 OFF）と、
    // 直した確定だけを残すか（キーの名前は macOS版の設定と同じ）
    bool conversionLog = false;
    bool conversionLogCorrectionsOnly = false;
    // 打ち間違いの訂正（既定 OFF。モデルは設定画面で ON にしたときに取得する）と、採用する margin のしきい値（0〜10）・
    // 入力が止まってから確かめるまでの時間（100〜2000ms）・訂正する読みの最低文字数（1〜20）。
    // キーの名前と既定値は macOS版の TypoNormalizerSettings と同じ
    bool typoCorrection = false;
    double typoThreshold = 2.5;
    int typoDelayMs = 500;
    int typoMinimumLength = 4;
};

Config LoadConfig(const std::filesystem::path& path);
bool SaveConfig(const std::filesystem::path& path, const Config& config);

} // namespace iroha
