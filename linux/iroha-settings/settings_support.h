#pragma once
// 設定画面（iroha-settings）のうち、画面に依存しない部分: タブの名前・変換モデルの一覧と取得・
// 設定ファイルの場所・ライセンスの一覧。GTK を使わないので単体テストする（tests/test_settings.cpp）

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "iroha/config.h"

namespace iroha::settings {

// ---- タブ ----

// タブの並び（Windows版の設定画面と同じ順に、Linux版で足した「情報」を最後に置く）
enum class Tab {
    Model = 0,
    Input,
    UserDictionary,
    TermDictionary,
    RewriteRules,
    AI,
    Selection,
    Learning,
    About,
    Count,
};
constexpr int kTabCount = static_cast<int>(Tab::Count);

// --tab に渡す名前（Windows版と同じ。about は Linux版で足した）
const char* TabName(Tab tab);
// タブの見出し
const char* TabTitle(Tab tab);
std::optional<Tab> ParseTabName(const std::string& name);

// ---- 置き場所 ----

std::filesystem::path ConfigPath();
std::filesystem::path ModelsDirectory();
std::filesystem::path UserDictionaryPath();
std::filesystem::path LearningPath();
std::filesystem::path RewriteRulesPath();
std::filesystem::path InputHistoryDirectory();
std::filesystem::path ConversionLogDirectory();

// ---- 変換モデル ----

// 設定画面から取得できる変換モデル（Windows版の設定画面の一覧と同じ。照合の値は、iroha-kkc-v1 は
// macOS版の ModelDownloader.swift と同じ、zenz は Hugging Face の配布元の値）
struct KnownModel {
    const char* label;
    const char* fileName;
    const char* url;
    uint64_t bytes;
    const char* sha256; // 16進小文字
};
const std::vector<KnownModel>& KnownModels();
// 今使うモデルのファイル名（config.json の model。空なら既定のモデル。IBus のエンジンと同じ決め方）
std::string CurrentModelFileName(const Config& config);
// 一覧のうち、今使っているモデルの位置（一覧に無ければ nullopt）
std::optional<size_t> CurrentModelIndex(const Config& config);

// 取得の進み具合（受け取ったバイト数・全体のバイト数）。偽を返すと取得を止める
using DownloadProgress = std::function<bool(uint64_t received, uint64_t total)>;

// モデルを <modelsDirectory>/<fileName> に取得する。同じフォルダの一時ファイル（.tmp）へ落とし、
// 大きさと SHA-256 を照合してから置く（照合できなければ置かずに一時ファイルを消す）
bool DownloadModel(const KnownModel& model, const std::filesystem::path& modelsDirectory,
                   const DownloadProgress& progress, std::string* error);
// ファイルの大きさと SHA-256 を照合する
bool VerifyFile(const std::filesystem::path& path, uint64_t bytes, const std::string& sha256, std::string* error);

// ---- ライセンス ----

struct LicenseEntry {
    std::string name;
    std::string holder;
    std::string license;
    std::string note; // 何に使っているか（無ければ空）
    std::string url;
    std::string licenseUrl; // ライセンス文書（無ければ空）
};
struct LicenseGroup {
    std::vector<LicenseEntry> entries;
    std::string footnote; // 一覧の下に出す説明（無ければ空）
};
// 情報タブのライセンスの一覧。iroha 本体と同梱のデータに、データフォルダに入れた変換モデル（zenz）・
// 打ち間違い訂正モデル・専門辞書・変換のベンチマークの評価セットのものを足す（後ろの 3 つは入れたときの記録から取る）
std::vector<LicenseGroup> Licenses(const std::filesystem::path& dataDirectory);

// ---- 表示の補助 ----

// 1234567 → "1.2 MB"
std::string FormatBytes(uint64_t bytes);

// 選択テキストの「マウスで選んだとき」の値（config の selectionTriggerMode。macOS版と同じ値）と、設定画面の並びの番号。
// 知らない値は既定の "bubble"
int SelectionTriggerIndex(const std::string& value);
std::string SelectionTriggerValue(int index);

} // namespace iroha::settings
