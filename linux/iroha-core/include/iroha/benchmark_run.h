#pragma once
// アプリ内ベンチマークの 1 回の計測の記録と、`iroha-bench` が標準出力に書く進み具合。
// 移植元: macos/Sources/IrohaCore/Benchmark/BenchmarkRun.swift（記録の形は 3 つの OS で同じ。
// キーと OS ごとの値の取り方は testdata/speed/README.md の「アプリ内のベンチマークの記録」）
//
// 記録は `<データフォルダ>/logs/bench/<日時>-<ホスト名>.json`（要約）と同じ名前の `.tsv`
// （項目ごとの時間。`IROHA_TIMINGS` と同じ列なので `testdata/speed/summarize.py` で読める）に置く。
// 文字列はすべて UTF-8

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "iroha/benchmark.h"

namespace iroha::benchmark_run {

// 辞書ラティスの使い方（`iroha-cli` の `IROHA_LATTICE` と同じ 2 条件。`training/README.md` の「比較の標準条件」）
enum class Lattice {
    Off,    // モデルだけ（ライブ変換の第一候補と同じ経路）
    Always, // 辞書ラティスの候補をモデルで採点し直し、第一候補もそこから選ぶ
};
// "off" / "always"
const char* LatticeName(Lattice lattice);
std::optional<Lattice> ParseLattice(const std::string& name);
// 「モデルだけ」「辞書ラティス + 再採点」
const char* LatticeDisplayName(Lattice lattice);

// 計測の条件
struct Condition {
    Lattice lattice = Lattice::Off;
    // 推論エンジン（"llama" / "mlx"）
    std::string backend = "llama";
    // モデルのファイル名
    std::string model;
    // LoRA アダプタのファイル名（無ければ無し）
    std::optional<std::string> adapter;
    // 推論の実行先。"CPU" か「ggml の装置名: 説明」（"Vulkan0: NVIDIA GeForce RTX 4060" など）。形式 1 の記録には無い
    std::optional<std::string> device;
    // llama.cpp が生成に使う CPU のスレッド数（llama_n_threads）。形式 1 の記録には無い
    std::optional<int> threads;

    // 「辞書ラティス + 再採点、llama.cpp（CPU、4 スレッド）、モデル …」の形の 1 行
    std::string SummaryText() const;
    bool operator==(const Condition& other) const {
        return lattice == other.lattice && backend == other.backend && model == other.model &&
               adapter == other.adapter && device == other.device && threads == other.threads;
    }
};

// 速度を左右する、計測した機械の状態。macOS版・Windows版・Linux版で同じ形にする。
// その OS で取れない項目は無しにする（推し量った値を入れない。Windows・Linux では発熱の状態が取れない）
struct Environment {
    // "macos" / "windows" / "linux"
    std::string platform;
    std::string appVersion;
    std::string hostName;
    // 機種（Linux は /sys/class/dmi/id の製造元と製品名）。分からなければ無し
    std::optional<std::string> machineModel;
    // CPU の名前
    std::string cpu;
    // 論理コアの数
    std::optional<int> cpuCores;
    std::string osVersion;
    // "ac" / "battery"。分からなければ無し
    std::optional<std::string> powerSource;
    // 省電力の設定が効いていたか。分からなければ無し
    std::optional<bool> powerSaving;
    // 計測の前後の発熱の状態（"nominal" / "fair" / "serious" / "critical"）。取れない OS では無し
    std::optional<std::string> thermalStateBefore;
    std::optional<std::string> thermalStateAfter;

    // 速度の数字が落ちている恐れがある状態（バッテリー・省電力・発熱）の説明。無ければ無し
    std::optional<std::string> ThrottlingNote() const;
    bool operator==(const Environment& other) const {
        return platform == other.platform && appVersion == other.appVersion && hostName == other.hostName &&
               machineModel == other.machineModel && cpu == other.cpu && cpuCores == other.cpuCores &&
               osVersion == other.osVersion && powerSource == other.powerSource &&
               powerSaving == other.powerSaving && thermalStateBefore == other.thermalStateBefore &&
               thermalStateAfter == other.thermalStateAfter;
    }
};

// 正解しなかった項目（設定画面で見返す）
struct Miss {
    std::string index;
    std::string context;
    std::string input;
    std::string output;
    std::vector<std::string> expected;
    bool operator==(const Miss& other) const {
        return index == other.index && context == other.context && input == other.input &&
               output == other.output && expected == other.expected;
    }
};

// 1 回の計測の記録（要約のファイル）
struct Record {
    // 記録の形式。2 で OS 共通の形にした（2026-11-14）。キーが無い記録は形式 1（macOS だけの形）
    static constexpr int kCurrentFormatVersion = 2;

    std::optional<int> formatVersion = kCurrentFormatVersion;
    // ファイル名（拡張子なし）
    std::string id;
    std::string benchmarkID;
    std::string benchmarkName;
    Condition condition;
    Environment environment;
    // 計測の始めと終わり（1970-01-01 UTC からの秒。ファイルには ISO 8601 で書く）
    int64_t startedAt = 0;
    int64_t finishedAt = 0;
    // モデルの読み込みを含む、最初の 1 回の変換の時間（ミリ秒）
    double warmUpMilliseconds = 0;
    // 計測中に入力があったか（あれば速度の数字は IME の変換と取り合った値）。確かめられない環境では無し
    std::optional<bool> inputDuringRun;
    benchmark::Summary summary;
    std::vector<Miss> misses;

    static std::vector<Miss> Misses(const std::vector<benchmark::Item>& items,
                                    const std::vector<benchmark::ItemResult>& results);
    bool operator==(const Record& other) const {
        return formatVersion == other.formatVersion && id == other.id && benchmarkID == other.benchmarkID &&
               benchmarkName == other.benchmarkName && condition == other.condition &&
               environment == other.environment && startedAt == other.startedAt &&
               finishedAt == other.finishedAt && warmUpMilliseconds == other.warmUpMilliseconds &&
               inputDuringRun == other.inputDuringRun && summary == other.summary && misses == other.misses;
    }
};

// 記録の JSON（macOS版の JSONEncoder の出力と同じキー。取れない項目はキーごと書かない）
std::string EncodeRecord(const Record& record);
// 形式 1（macOS版の最初の版。environment.chip・lowPowerMode）も読む
std::optional<Record> DecodeRecord(const std::string& json);
std::string EncodeEnvironment(const Environment& environment);
std::optional<Environment> DecodeEnvironment(const std::string& json);
std::string EncodeSummary(const benchmark::Summary& summary);

// 日時（1970-01-01 UTC からの秒）と ISO 8601（「2026-11-14T12:55:32Z」）
std::string FormatIso8601(int64_t seconds);
std::optional<int64_t> ParseIso8601(const std::string& text);

// ---- 置き場所 ----

// <データフォルダ>/logs/bench
std::filesystem::path Directory(const std::filesystem::path& dataDirectory);
std::filesystem::path RecordPath(const std::filesystem::path& dataDirectory, const std::string& id);
std::filesystem::path TimingsPath(const std::filesystem::path& dataDirectory, const std::string& id);

// 記録の名前（「20261114-153012-<ホスト名>」。日時はその機械の地方時、ホスト名の英数字とハイフン以外は「_」）
std::string MakeId(int64_t seconds, const std::string& hostName);

bool Write(const std::filesystem::path& dataDirectory, const Record& record,
           const std::vector<benchmark::ItemResult>& results, std::string* error);
// 残っている記録。新しい順
std::vector<Record> Records(const std::filesystem::path& dataDirectory);
void Delete(const std::filesystem::path& dataDirectory, const std::string& id);

// ---- iroha-bench の進み具合（標準出力の JSON Lines、1 行 1 件） ----

struct Event {
    enum class Kind {
        Loading,  // モデルの読み込み中（最初の 1 回の変換）
        Progress, // 項目を 1 つ測り終えた
        Finished, // 計測を終えて記録を書いた
        Failed,
    };
    Kind kind = Kind::Progress;
    std::optional<int> done;
    std::optional<int> total;
    std::optional<int> hits;
    // finished のときの記録の ID
    std::optional<std::string> recordID;
    std::optional<std::string> message;

    // 1 行の JSON（キーは名前の順、末尾に改行）
    std::string JsonLine() const;
    static std::optional<Event> Parse(const std::string& line);
    bool operator==(const Event& other) const {
        return kind == other.kind && done == other.done && total == other.total && hits == other.hits &&
               recordID == other.recordID && message == other.message;
    }
};

// ---- 結果の Markdown（設定画面の「Markdown でコピー」） ----

std::string Markdown(const Record& record);
// 地方時の「2026/11/14 21:55」（設定画面とMarkdownの見出しに使う）
std::string FormatLocalDateTime(int64_t seconds);

} // namespace iroha::benchmark_run
