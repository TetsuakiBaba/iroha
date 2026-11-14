#pragma once
// 変換のベンチマーク: 評価セットの読み込み・採点・集計（testdata/speed/summarize.py と同じ定義）・記録・取得。
// 移植元: macos/Tests/IrohaCoreTests/BenchmarkTests.swift（テストは 1 対 1）。
// test_main.cpp からインクルードされる（テストマクロを共有するため）

#include <cmath>
#include <fstream>
#include <map>

#include "benchmark_fetcher.h"
#include "iroha/benchmark.h"
#include "iroha/benchmark_catalog.h"
#include "iroha/benchmark_run.h"
#include "iroha/user_dictionary.h"
#include "term_dictionary_fetcher.h"

namespace {

// 読みをそのまま返すか、表から引いて返すだけのエンジン
class BenchmarkTableEngine : public iroha::ConversionEngine {
public:
    explicit BenchmarkTableEngine(std::map<std::u32string, std::u32string> table) : table_(std::move(table)) {}
    bool Convert(const std::u32string& reading, const std::u32string&, int, std::vector<std::u32string>* results,
                 std::string*) override {
        const auto found = table_.find(reading);
        results->assign(1, found != table_.end() ? found->second : reading);
        return true;
    }

private:
    std::map<std::u32string, std::u32string> table_;
};

// 一時的なデータフォルダ（抜けると消す）
struct BenchmarkTemporaryDirectory {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("iroha-bench-" + iroha::GenerateUuid());
    std::filesystem::path data = root / "data";
    BenchmarkTemporaryDirectory() { std::filesystem::create_directories(data); }
    ~BenchmarkTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

std::vector<std::string> IndicesOf(const std::vector<iroha::benchmark::Item>& items) {
    std::vector<std::string> indices;
    for (const auto& item : items) indices.push_back(item.index);
    return indices;
}

// ---- 読み込み ----

void TestBenchmarkLoadsJSONArrayAndJSONLines() {
    const std::string array =
        "[{\"index\": \"a-1\", \"context_text\": \"\", \"input\": \"キョウ\", \"expected_output\": [\"今日\"]},\n"
        " {\"index\": \"a-2\", \"context_text\": \"前の\", \"input\": \"ハナシ\", \"expected_output\": [\"話\", "
        "\"はなし\"]}]";
    std::vector<iroha::benchmark::Item> items;
    std::string error;
    EXPECT_TRUE(iroha::benchmark::LoadItems(array, &items, &error));
    EXPECT_TRUE((IndicesOf(items) == std::vector<std::string>{"a-1", "a-2"}));
    EXPECT_TRUE(!items[0].source);
    EXPECT_TRUE((items[1].expectedOutput == std::vector<std::u32string>{U"話", U"はなし"}));

    // 配布用の JSON Lines（出典・ライセンスなど余分なキーがあり、空行・CRLF もありうる）
    const std::string lines =
        "{\"index\": \"b-1\", \"source\": \"jmrd\", \"context_text\": \"\", \"input\": \"キョウ\", "
        "\"expected_output\": [\"今日\"], \"license\": \"CC BY-SA 4.0\"}\r\n"
        "\n"
        "{\"index\": \"b-2\", \"source\": \"mrmp\", \"context_text\": \"x\", \"input\": \"ハナシ\", "
        "\"expected_output\": [\"話\"]}";
    std::vector<iroha::benchmark::Item> parsed;
    EXPECT_TRUE(iroha::benchmark::LoadItems(lines, &parsed, &error));
    EXPECT_TRUE((IndicesOf(parsed) == std::vector<std::string>{"b-1", "b-2"}));
    EXPECT_TRUE(parsed.size() == 2 && parsed[0].source == std::optional<std::string>("jmrd") &&
                parsed[1].source == std::optional<std::string>("mrmp"));
}

void TestBenchmarkRejectsBrokenOrEmptySets() {
    std::vector<iroha::benchmark::Item> items;
    std::string error;
    EXPECT_TRUE(!iroha::benchmark::LoadItems(std::string("\n\n"), &items, &error));
    EXPECT_TRUE(error == "評価セットに項目がありません");
    const std::string broken =
        "{\"index\": \"a\", \"context_text\": \"\", \"input\": \"ア\", \"expected_output\": [\"亜\"]}\n{oops}\n";
    EXPECT_TRUE(!iroha::benchmark::LoadItems(broken, &items, &error));
    EXPECT_TRUE(error == "評価セットの 2 行目が読めません");
}

// ---- 採点 ----

void TestBenchmarkScoreUsesClosestReference() {
    const auto hit = iroha::benchmark::ScoreOutput(U"今日は", {U"きょうは", U"今日は"});
    EXPECT_TRUE(hit.hit);
    EXPECT_TRUE(hit.minCER == 0);

    // 「京は」は「今日は」まで 2 文字（2/3）、「きょうは」まで 3 文字（3/4）。近いほうを取る
    const auto miss = iroha::benchmark::ScoreOutput(U"京は", {U"きょうは", U"今日は"});
    EXPECT_TRUE(!miss.hit);
    EXPECT_TRUE(std::fabs(miss.minCER - 2.0 / 3.0) < 1e-9);
}

void TestBenchmarkEditDistance() {
    EXPECT_TRUE(iroha::benchmark::EditDistance(U"かきくけこ", U"かくけこさ") == 2);
    EXPECT_TRUE(iroha::benchmark::EditDistance(U"", U"あい") == 2);
    EXPECT_TRUE(iroha::benchmark::EditDistance(U"同じ", U"同じ") == 0);
}

// ---- 集計 ----

// summarize.py の percentile（最近順位法）と同じ値になる
void TestBenchmarkPercentileMatchesSummarizeScript() {
    std::vector<double> values;
    for (int i = 1; i <= 10; ++i) values.push_back(i);
    EXPECT_TRUE(iroha::benchmark::Latency::Percentile(values, 50) == 5);
    EXPECT_TRUE(iroha::benchmark::Latency::Percentile(values, 90) == 9);
    EXPECT_TRUE(iroha::benchmark::Latency::Percentile(values, 99) == 10);
    EXPECT_TRUE(iroha::benchmark::Latency::Percentile({7}, 50) == 7);
    const auto latency = iroha::benchmark::Latency::Of({3, 1, 2});
    EXPECT_TRUE(latency.mean == 2);
    EXPECT_TRUE(latency.p50 == 2);
    EXPECT_TRUE(latency.max == 3);
}

void TestBenchmarkRunAndSummarize() {
    const std::vector<iroha::benchmark::Item> items = {
        {"1", std::string("s1"), U"", U"キョウ", {U"今日"}},
        {"2", std::string("s1"), U"前", U"ハナシ", {U"話"}},
        {"3", std::string("s2"), U"", std::u32string(25, U'ア'), {U"亜"}},
    };
    BenchmarkTableEngine engine({{U"キョウ", U"今日"}, {U"ハナシ", U"放し"}});
    std::vector<iroha::benchmark::ItemResult> results;
    for (const auto& item : items) results.push_back(iroha::benchmark::Run(item, engine));
    EXPECT_TRUE(results.size() == 3 && results[0].hit && !results[1].hit && !results[2].hit);
    EXPECT_TRUE(results[0].readingLength == 3 && results[1].readingLength == 3 && results[2].readingLength == 25);

    const auto summary = iroha::benchmark::Summary::Of(results);
    EXPECT_TRUE(summary.total.count == 3);
    EXPECT_TRUE(summary.total.hits == 1);
    EXPECT_TRUE(summary.withContext.count == 1);
    EXPECT_TRUE(summary.withoutContext.hits == 1);
    EXPECT_TRUE(summary.perSource.size() == 2 && summary.perSource[0].source == "s1" &&
                summary.perSource[1].source == "s2");
    EXPECT_TRUE(!summary.perSource.empty() && summary.perSource[0].tally.hits == 1);
    // 1〜10 に 2 件、21〜30 に 1 件。項目の無い区切りは出さない
    EXPECT_TRUE(summary.lengthBins.size() == 2 && summary.lengthBins[0].Label() == "1〜10" &&
                summary.lengthBins[1].Label() == "21〜30");
    EXPECT_TRUE(summary.lengthBins.size() == 2 && summary.lengthBins[0].count == 2 && summary.lengthBins[1].count == 1);

    const auto misses = iroha::benchmark_run::Record::Misses(items, results);
    EXPECT_TRUE(misses.size() == 2 && misses[0].index == "2" && misses[1].index == "3");
    EXPECT_TRUE(!misses.empty() && misses[0].output == "放し");
}

void TestBenchmarkTimingsTSVHasSummarizeColumns() {
    const iroha::benchmark::Item item{"x", std::nullopt, U"ab", U"アイウ", {U"愛う"}};
    iroha::InferenceStats stats;
    stats.neuralNetworkMicros = 2000;
    stats.neuralNetworkCalls = 3;
    const auto result = iroha::benchmark::ItemResult::Make(item, U"愛う", 4.5, &stats);
    const std::string tsv = iroha::benchmark::TimingsTsv({result});
    const size_t newline = tsv.find('\n');
    EXPECT_TRUE(tsv.substr(0, newline) == "index\treading_length\tcontext_length\tms\tnn_ms\tnn_calls");
    EXPECT_TRUE(tsv.substr(newline + 1) == "x\t3\t2\t4.500\t2.000\t3\n");
}

// ---- 記録と進み具合 ----

void TestBenchmarkRecordRoundTripAndEvents() {
    BenchmarkTemporaryDirectory directory;
    const iroha::benchmark::Item item{"1", std::nullopt, U"", U"キョウ", {U"今日"}};
    BenchmarkTableEngine engine({});
    const auto result = iroha::benchmark::Run(item, engine);
    iroha::benchmark_run::Environment environment;
    environment.platform = "macos";
    environment.appVersion = "1.0";
    environment.hostName = "test";
    environment.machineModel = "Mac";
    environment.cpu = "Apple M";
    environment.cpuCores = 8;
    environment.osVersion = "27";
    environment.powerSource = "ac";
    environment.powerSaving = false;
    environment.thermalStateBefore = "nominal";
    environment.thermalStateAfter = "fair";
    const int64_t started = 1800000000;
    const std::string id = iroha::benchmark_run::MakeId(started, "my mac.local");
    EXPECT_TRUE(id.size() > 13 && id.substr(id.size() - 13) == "-my_mac_local");
    iroha::benchmark_run::Record record;
    record.id = id;
    record.benchmarkID = "set";
    record.benchmarkName = "Set";
    record.condition.lattice = iroha::benchmark_run::Lattice::Always;
    record.condition.backend = "llama";
    record.condition.model = "m.gguf";
    record.condition.device = "MTL0: Apple M";
    record.condition.threads = 4;
    record.environment = environment;
    record.startedAt = started;
    record.finishedAt = started + 5;
    record.warmUpMilliseconds = 100;
    record.inputDuringRun = true;
    record.summary = iroha::benchmark::Summary::Of({result});
    record.misses = iroha::benchmark_run::Record::Misses({item}, {result});
    std::string error;
    EXPECT_TRUE(iroha::benchmark_run::Write(directory.data, record, {result}, &error));

    const auto records = iroha::benchmark_run::Records(directory.data);
    EXPECT_TRUE(records.size() == 1 && records[0] == record);
    std::ifstream file(iroha::benchmark_run::TimingsPath(directory.data, id), std::ios::binary);
    const std::string tsv((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    EXPECT_TRUE(tsv.rfind(iroha::benchmark::kTimingsHeader, 0) == 0);
    EXPECT_TRUE(record.formatVersion == iroha::benchmark_run::Record::kCurrentFormatVersion);
    const std::string markdown = iroha::benchmark_run::Markdown(record);
    EXPECT_TRUE(markdown.find("| 全体 | 1 | 0 (0.0%) |") != std::string::npos);
    EXPECT_TRUE(markdown.find("llama.cpp（MTL0: Apple M、4 スレッド）") != std::string::npos);
    EXPECT_TRUE(environment.ThrottlingNote() == std::optional<std::string>("発熱の状態 nominal → fair"));
    iroha::benchmark_run::Delete(directory.data, id);
    EXPECT_TRUE(iroha::benchmark_run::Records(directory.data).empty());

    iroha::benchmark_run::Event event;
    event.kind = iroha::benchmark_run::Event::Kind::Progress;
    event.done = 3;
    event.total = 10;
    event.hits = 2;
    const std::string line = event.JsonLine();
    EXPECT_TRUE(!line.empty() && line.back() == '\n');
    EXPECT_TRUE(iroha::benchmark_run::Event::Parse(line.substr(0, line.size() - 1)) ==
                std::optional<iroha::benchmark_run::Event>(event));
    EXPECT_TRUE(!iroha::benchmark_run::Event::Parse("llama.cpp のログ"));
}

// 形式 1（最初の版。macOS だけの名前 chip・lowPowerMode）の記録も読める
void TestBenchmarkReadsFormatOneRecord() {
    const std::string old =
        "{\"benchmarkID\": \"iroha-bench-v1-quick\", \"benchmarkName\": \"iroha-bench-v1（抜粋）\",\n"
        " \"condition\": {\"backend\": \"llama\", \"lattice\": \"off\", \"model\": \"m.gguf\"},\n"
        " \"environment\": {\"appVersion\": \"0.17.1\", \"chip\": \"Apple M2\", \"hostName\": \"bell\", "
        "\"lowPowerMode\": true,\n"
        "                 \"machineModel\": \"Mac14,2\", \"osVersion\": \"Version 27.0\", \"powerSource\": \"ac\",\n"
        "                 \"thermalStateAfter\": \"nominal\", \"thermalStateBefore\": \"nominal\"},\n"
        " \"finishedAt\": \"2026-11-14T12:56:12Z\", \"id\": \"20261114-215532-bell\", \"inputDuringRun\": false,\n"
        " \"misses\": [], \"startedAt\": \"2026-11-14T12:55:32Z\", \"warmUpMilliseconds\": 300,\n"
        " \"summary\": " +
        iroha::benchmark_run::EncodeSummary(iroha::benchmark::Summary::Of({})) + "}";
    const auto record = iroha::benchmark_run::DecodeRecord(old);
    EXPECT_TRUE(record.has_value());
    if (!record) return;
    EXPECT_TRUE(!record->formatVersion);
    EXPECT_TRUE(record->environment.platform == "macos");
    EXPECT_TRUE(record->environment.cpu == "Apple M2");
    EXPECT_TRUE(record->environment.powerSaving == std::optional<bool>(true));
    EXPECT_TRUE(!record->environment.cpuCores);
    EXPECT_TRUE(!record->condition.device);
    EXPECT_TRUE(record->inputDuringRun == std::optional<bool>(false));
    EXPECT_TRUE(record->environment.ThrottlingNote() == std::optional<std::string>("省電力の設定"));

    // 書き直すと新しい名前だけになる
    const std::string json = iroha::benchmark_run::EncodeEnvironment(record->environment);
    EXPECT_TRUE(json.find("\"cpu\"") != std::string::npos);
    EXPECT_TRUE(json.find("chip") == std::string::npos);
    EXPECT_TRUE(json.find("lowPowerMode") == std::string::npos);
}

// 取れない項目（Linux の発熱・Wayland のキー入力など）は書かない
void TestBenchmarkOmitsUnknownFields() {
    iroha::benchmark_run::Environment environment;
    environment.platform = "linux";
    environment.appVersion = "1.0";
    environment.hostName = "gpu";
    environment.cpu = "AMD EPYC";
    environment.cpuCores = 64;
    environment.osVersion = "Ubuntu 24.04";
    const std::string json = iroha::benchmark_run::EncodeEnvironment(environment);
    EXPECT_TRUE(json.find("thermalState") == std::string::npos);
    EXPECT_TRUE(json.find("powerSaving") == std::string::npos);
    EXPECT_TRUE(!environment.ThrottlingNote());
    EXPECT_TRUE(iroha::benchmark_run::DecodeEnvironment(json) == std::optional<iroha::benchmark_run::Environment>(environment));
}

// ---- 取得 ----

void TestBenchmarkInstallChecksSizeAndChecksum() {
    BenchmarkTemporaryDirectory directory;
    const std::string body = "{\"index\": \"1\", \"context_text\": \"\", \"input\": \"ア\", \"expected_output\": [\"亜\"]}\n";
    const std::filesystem::path source = directory.root / "set.jsonl";
    {
        std::ofstream file(source, std::ios::binary);
        file << body;
    }
    std::string url = "file:///" + source.generic_u8string();
    iroha::BenchmarkCatalog::Entry good;
    good.id = "test-set";
    good.name = "テスト";
    good.items = 1;
    good.file = {url, body.size(), iroha::TermDictionaryFetcher::Sha256Hex(source)};
    good.license = "CC0";
    good.sources = {{"出典", "誰か", "CC0", "https://example.com"}};
    iroha::BenchmarkInstall::Record record;
    std::string error;
    EXPECT_TRUE(iroha::BenchmarkFetcher::Install(good, directory.data, &record, &error));
    EXPECT_TRUE(record.items == 1);
    auto installed = iroha::BenchmarkInstall::InstalledRecords(directory.data);
    EXPECT_TRUE(installed.size() == 1 && installed[0].id == "test-set");
    std::vector<iroha::benchmark::Item> items;
    EXPECT_TRUE(iroha::benchmark::LoadItems(iroha::BenchmarkInstall::FilePath(directory.data, "test-set"), &items,
                                            &error) &&
                items.size() == 1);

    iroha::BenchmarkCatalog::Entry broken = good;
    broken.id = "broken";
    broken.name = "壊れた";
    broken.file.sha256 = std::string(64, '0');
    broken.sources.clear();
    EXPECT_TRUE(!iroha::BenchmarkFetcher::Install(broken, directory.data, &record, &error));
    EXPECT_TRUE(error == "評価セットが壊れています（チェックサム不一致）。もう一度お試しください");
    installed = iroha::BenchmarkInstall::InstalledRecords(directory.data);
    EXPECT_TRUE(installed.size() == 1 && installed[0].id == "test-set");

    EXPECT_TRUE(iroha::BenchmarkInstall::Remove(directory.data, "test-set"));
    EXPECT_TRUE(iroha::BenchmarkInstall::InstalledRecords(directory.data).empty());
}

void RunBenchmarkTests() {
    TestBenchmarkLoadsJSONArrayAndJSONLines();
    TestBenchmarkRejectsBrokenOrEmptySets();
    TestBenchmarkScoreUsesClosestReference();
    TestBenchmarkEditDistance();
    TestBenchmarkPercentileMatchesSummarizeScript();
    TestBenchmarkRunAndSummarize();
    TestBenchmarkTimingsTSVHasSummarizeColumns();
    TestBenchmarkRecordRoundTripAndEvents();
    TestBenchmarkReadsFormatOneRecord();
    TestBenchmarkOmitsUnknownFields();
    TestBenchmarkInstallChecksSizeAndChecksum();
}

} // namespace
