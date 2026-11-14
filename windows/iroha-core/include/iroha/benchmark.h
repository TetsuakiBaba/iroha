#pragma once
// 変換の精度と速度のベンチマーク（評価セットを 1 件ずつ変換して、正解との一致と時間を測る）。
// 移植元: macos/Sources/IrohaCore/Benchmark/Benchmark.swift・BenchmarkSummary.swift（挙動互換を維持すること）
//
// `iroha-cli ajimee` と、設定画面から起動する `iroha-bench`（設定 > モデル > 変換のベンチマーク）が同じ実装を使う。
// 採点は AJIMEE-Bench（azooKey/AJIMEE-Bench）と同じ: 第一候補を許容解のリストと照合し、acc@1（どれかと完全に一致）と
// MinCER（許容解ごとの CER = 編集距離 / 正解の長さ、の最小値を項目ごとに取って平均）を出す。
// 速度の集計は `testdata/speed/summarize.py` と同じ定義（最近順位法のパーセンタイル・読みの文字数の区切り）。
//
// 文字数と編集距離は Unicode のコードポイントで数える（macOS版は書記素。評価セットの読みはカタカナなので同じ値になる）

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "iroha/conversion_engine.h"
#include "iroha/inference_timer.h"

namespace iroha::benchmark {

// 評価セットの 1 項目（AJIMEE-Bench と iroha-bench-v1 の共通の形）
struct Item {
    std::string index;
    // 出典（iroha-bench-v1 の `source`。AJIMEE には無い）
    std::optional<std::string> source;
    std::u32string contextText;
    // 読み（カタカナ）
    std::u32string input;
    std::vector<std::u32string> expectedOutput;
};

// 評価セットを読む。JSON の配列（`evaluation_items.json`）と JSON Lines（配布用の `.jsonl`）のどちらも受け付ける。
// 読めなければ偽で、error は「評価セットに項目がありません」か「評価セットの N 行目が読めません」
bool LoadItems(const std::string& data, std::vector<Item>* items, std::string* error);
bool LoadItems(const std::filesystem::path& path, std::vector<Item>* items, std::string* error);

// ---- 採点 ----

struct Score {
    bool hit = false;
    double minCER = 1.0;
};
Score ScoreOutput(const std::u32string& output, const std::vector<std::u32string>& expected);
// 文字単位の編集距離（レーベンシュタイン距離）
int EditDistance(const std::u32string& a, const std::u32string& b);

// ---- 1 項目の結果 ----

// 1 項目を変換した結果。時間はミリ秒
struct ItemResult {
    std::string index;
    std::optional<std::string> source;
    int readingLength = 0;
    int contextLength = 0;
    std::u32string output;
    bool hit = false;
    double minCER = 1.0;
    // 変換 1 回の時間（エンジンの呼び出しの前後）
    double milliseconds = 0;
    // NN の計算の合計と回数（inference_timer。macOS版の prompt・decode・select の和にあたる）
    double neuralNetworkMilliseconds = 0;
    int neuralNetworkCalls = 0;

    static ItemResult Make(const Item& item, const std::u32string& output, double milliseconds,
                           const InferenceStats* stats);
};

// 1 項目を変換して測る。変換に失敗したら読みをそのまま答えとして扱う（`iroha-cli ajimee` と同じ）。
// 失敗したときは error に理由を入れる
ItemResult Run(const Item& item, ConversionEngine& engine, std::string* error = nullptr);
// 計測の前に 1 回変換して、モデルの読み込みを計測から外す。読み込みにかかった時間（ミリ秒）を返す
double WarmUp(ConversionEngine& engine);

// ---- 集計 ----

// 精度の集計（acc@1 と MinCER）
struct Tally {
    int count = 0;
    int hits = 0;
    double minCERSum = 0;

    void Add(const ItemResult& result);
    // acc@1（0〜1）
    double Accuracy() const { return count > 0 ? static_cast<double>(hits) / count : 0; }
    // MinCER の平均（0〜1）
    double MeanMinCER() const { return count > 0 ? minCERSum / count : 0; }
    // 「acc@1 172/200 (86.0%)  MinCER 2.10%」（`iroha-cli ajimee` の表示）
    std::string SummaryText() const;
    bool operator==(const Tally& other) const {
        return count == other.count && hits == other.hits && minCERSum == other.minCERSum;
    }
};

// 変換時間の集計（ミリ秒）。`testdata/speed/summarize.py` の `stats` と同じ
struct Latency {
    int count = 0;
    double mean = 0;
    double p50 = 0;
    double p90 = 0;
    double p99 = 0;
    double max = 0;

    static Latency Of(std::vector<double> milliseconds);
    // 最近順位法（nearest-rank）。p は 0〜100。`summarize.py` の `percentile` と同じ。sorted は昇順
    static double Percentile(const std::vector<double>& sorted, double p);
    bool operator==(const Latency& other) const {
        return count == other.count && mean == other.mean && p50 == other.p50 && p90 == other.p90 &&
               p99 == other.p99 && max == other.max;
    }
};

// 読みの文字数の区切り 1 つと、その中の中央値
struct LengthBin {
    int lower = 0;
    // 無ければ上限なし
    std::optional<int> upper;
    int count = 0;
    double p50 = 0;

    // 「1〜10」「41〜」（UTF-8）
    std::string Label() const;
    bool operator==(const LengthBin& other) const {
        return lower == other.lower && upper == other.upper && count == other.count && p50 == other.p50;
    }
};

// 読みの文字数の区切り（`summarize.py` の `LENGTH_BINS`）
extern const std::vector<std::pair<int, std::optional<int>>> kLengthBins;

struct SourceTally {
    std::string source;
    Tally tally;
    bool operator==(const SourceTally& other) const { return source == other.source && tally == other.tally; }
};

// 1 回の計測の集計
struct Summary {
    Tally total;
    Tally withContext;
    Tally withoutContext;
    // 出典ごと（出典の名前の順）。出典の無い評価セット（AJIMEE）では空
    std::vector<SourceTally> perSource;
    Latency latency;
    std::vector<LengthBin> lengthBins;
    // 変換時間のうち NN の計算が占める割合（0〜1）。内訳が取れなかったら無し
    std::optional<double> neuralNetworkShare;

    static Summary Of(const std::vector<ItemResult>& results);
    bool operator==(const Summary& other) const {
        return total == other.total && withContext == other.withContext && withoutContext == other.withoutContext &&
               perSource == other.perSource && latency == other.latency && lengthBins == other.lengthBins &&
               neuralNetworkShare == other.neuralNetworkShare;
    }
};

// ---- 時間の TSV ----

// `IROHA_TIMINGS` と同じ列（`testdata/speed/summarize.py` で読める。先頭の 4 列は 3 つの OS で同じ、
// 後ろの nn_ms・nn_calls は Windows版・Linux版の列）
extern const char kTimingsHeader[];
std::string TimingsLine(const ItemResult& result);
std::string TimingsTsv(const std::vector<ItemResult>& results);

} // namespace iroha::benchmark
