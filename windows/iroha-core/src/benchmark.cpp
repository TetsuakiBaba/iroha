#include "iroha/benchmark.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

#include "iroha/json.h"
#include "iroha/unicode.h"

namespace iroha::benchmark {

namespace {

// 1 項目の JSON を読む（macOS版の Codable と同じ: index・context_text・input・expected_output は必須、source は任意）
bool ParseItem(const json::Value& value, Item* item) {
    if (value.type != json::Value::Type::Object) return false;
    const json::Value* index = value.Find("index");
    const json::Value* context = value.Find("context_text");
    const json::Value* input = value.Find("input");
    const json::Value* expected = value.Find("expected_output");
    if (!index || index->type != json::Value::Type::String || !context ||
        context->type != json::Value::Type::String || !input || input->type != json::Value::Type::String ||
        !expected || expected->type != json::Value::Type::Array) {
        return false;
    }
    Item parsed;
    parsed.index = index->string;
    if (const json::Value* source = value.Find("source")) {
        if (source->type == json::Value::Type::String) {
            parsed.source = source->string;
        } else if (source->type != json::Value::Type::Null) {
            return false;
        }
    }
    parsed.contextText = Utf8ToUtf32(context->string);
    parsed.input = Utf8ToUtf32(input->string);
    for (const json::Value& reference : expected->array) {
        if (reference.type != json::Value::Type::String) return false;
        parsed.expectedOutput.push_back(Utf8ToUtf32(reference.string));
    }
    *item = std::move(parsed);
    return true;
}

bool IsBlank(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

bool LoadItems(const std::string& data, std::vector<Item>* items, std::string* error) {
    std::vector<Item> parsed;
    const size_t first = std::find_if_not(data.begin(), data.end(), IsBlank) - data.begin();
    if (first < data.size() && data[first] == '[') {
        json::Value root;
        if (!json::Parse(data, &root) || root.type != json::Value::Type::Array) {
            if (error) *error = "評価セットの 1 行目が読めません";
            return false;
        }
        for (const json::Value& value : root.array) {
            Item item;
            if (!ParseItem(value, &item)) {
                if (error) *error = "評価セットの 1 行目が読めません";
                return false;
            }
            parsed.push_back(std::move(item));
        }
    } else {
        size_t start = 0;
        int lineNumber = 0;
        while (start <= data.size()) {
            size_t end = data.find('\n', start);
            if (end == std::string::npos) end = data.size();
            ++lineNumber;
            const std::string line = data.substr(start, end - start);
            start = end + 1;
            if (std::all_of(line.begin(), line.end(), IsBlank)) {
                if (end == data.size()) break;
                continue;
            }
            json::Value value;
            Item item;
            if (!json::Parse(line, &value) || !ParseItem(value, &item)) {
                if (error) *error = "評価セットの " + std::to_string(lineNumber) + " 行目が読めません";
                return false;
            }
            parsed.push_back(std::move(item));
            if (end == data.size()) break;
        }
    }
    if (parsed.empty()) {
        if (error) *error = "評価セットに項目がありません";
        return false;
    }
    *items = std::move(parsed);
    return true;
}

bool LoadItems(const std::filesystem::path& path, std::vector<Item>* items, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "評価セットのファイルが読めません";
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return LoadItems(buffer.str(), items, error);
}

// ---- 採点 ----

int EditDistance(const std::u32string& a, const std::u32string& b) {
    if (a.empty()) return static_cast<int>(b.size());
    if (b.empty()) return static_cast<int>(a.size());
    std::vector<int> previous(b.size() + 1);
    std::vector<int> current(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) previous[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        current[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); ++j) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
        }
        std::swap(previous, current);
    }
    return previous[b.size()];
}

Score ScoreOutput(const std::u32string& output, const std::vector<std::u32string>& expected) {
    Score score;
    score.hit = std::find(expected.begin(), expected.end(), output) != expected.end();
    if (expected.empty()) return score;
    double minimum = 0;
    bool first = true;
    for (const std::u32string& reference : expected) {
        const double cer = static_cast<double>(EditDistance(output, reference)) /
                           static_cast<double>(std::max<size_t>(reference.size(), 1));
        if (first || cer < minimum) minimum = cer;
        first = false;
    }
    score.minCER = minimum;
    return score;
}

// ---- 1 項目の結果 ----

ItemResult ItemResult::Make(const Item& item, const std::u32string& output, double milliseconds,
                            const InferenceStats* stats) {
    const Score score = ScoreOutput(output, item.expectedOutput);
    ItemResult result;
    result.index = item.index;
    result.source = item.source;
    result.readingLength = static_cast<int>(item.input.size());
    result.contextLength = static_cast<int>(item.contextText.size());
    result.output = output;
    result.hit = score.hit;
    result.minCER = score.minCER;
    result.milliseconds = milliseconds;
    if (stats) {
        result.neuralNetworkMilliseconds = static_cast<double>(stats->neuralNetworkMicros) / 1000.0;
        result.neuralNetworkCalls = stats->neuralNetworkCalls;
    }
    return result;
}

ItemResult Run(const Item& item, ConversionEngine& engine, std::string* error) {
    InferenceStats stats;
    std::vector<std::u32string> candidates;
    std::u32string output = item.input;
    const auto start = std::chrono::steady_clock::now();
    {
        inference_timer::Scope scope(&stats);
        std::string failure;
        if (engine.Convert(item.input, item.contextText, 1, &candidates, &failure)) {
            if (!candidates.empty()) output = candidates.front();
        } else if (error) {
            *error = failure;
        }
    }
    const double elapsed = MillisecondsSince(start);
    return ItemResult::Make(item, output, elapsed, &stats);
}

double WarmUp(ConversionEngine& engine) {
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::u32string> ignored;
    std::string error;
    engine.Convert(U"うぉーむあっぷ", U"", 1, &ignored, &error);
    return MillisecondsSince(start);
}

// ---- 集計 ----

void Tally::Add(const ItemResult& result) {
    ++count;
    if (result.hit) ++hits;
    minCERSum += result.minCER;
}

std::string Tally::SummaryText() const {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "acc@1 %d/%d (%.1f%%)  MinCER %.2f%%", hits, count, Accuracy() * 100,
                  MeanMinCER() * 100);
    return buffer;
}

double Latency::Percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0;
    const int rank = std::max(1, static_cast<int>(std::ceil(p / 100 * static_cast<double>(sorted.size()))));
    return sorted[static_cast<size_t>(std::min<int>(rank, static_cast<int>(sorted.size())) - 1)];
}

Latency Latency::Of(std::vector<double> milliseconds) {
    std::sort(milliseconds.begin(), milliseconds.end());
    Latency latency;
    latency.count = static_cast<int>(milliseconds.size());
    if (!milliseconds.empty()) {
        double sum = 0;
        for (double value : milliseconds) sum += value;
        latency.mean = sum / static_cast<double>(milliseconds.size());
        latency.max = milliseconds.back();
    }
    latency.p50 = Percentile(milliseconds, 50);
    latency.p90 = Percentile(milliseconds, 90);
    latency.p99 = Percentile(milliseconds, 99);
    return latency;
}

std::string LengthBin::Label() const {
    return std::to_string(lower) + "\xE3\x80\x9C" + (upper ? std::to_string(*upper) : std::string()); // 〜
}

const std::vector<std::pair<int, std::optional<int>>> kLengthBins = {
    {1, 10}, {11, 20}, {21, 30}, {31, 40}, {41, std::nullopt}};

Summary Summary::Of(const std::vector<ItemResult>& results) {
    Summary summary;
    std::map<std::string, Tally> bySource;
    std::vector<double> times;
    double totalTime = 0;
    double neuralNetwork = 0;
    for (const ItemResult& result : results) {
        summary.total.Add(result);
        (result.contextLength > 0 ? summary.withContext : summary.withoutContext).Add(result);
        if (result.source) bySource[*result.source].Add(result);
        times.push_back(result.milliseconds);
        totalTime += result.milliseconds;
        neuralNetwork += result.neuralNetworkMilliseconds;
    }
    for (const auto& [source, tally] : bySource) summary.perSource.push_back({source, tally});
    summary.latency = Latency::Of(times);
    for (const auto& [lower, upper] : kLengthBins) {
        std::vector<double> part;
        for (const ItemResult& result : results) {
            if (result.readingLength >= lower && (!upper || result.readingLength <= *upper)) {
                part.push_back(result.milliseconds);
            }
        }
        if (part.empty()) continue;
        LengthBin bin;
        bin.lower = lower;
        bin.upper = upper;
        bin.count = static_cast<int>(part.size());
        bin.p50 = Latency::Of(part).p50;
        summary.lengthBins.push_back(bin);
    }
    if (totalTime > 0 && neuralNetwork > 0) summary.neuralNetworkShare = neuralNetwork / totalTime;
    return summary;
}

// ---- 時間の TSV ----

const char kTimingsHeader[] = "index\treading_length\tcontext_length\tms\tnn_ms\tnn_calls\n";

std::string TimingsLine(const ItemResult& result) {
    char numbers[96];
    std::snprintf(numbers, sizeof(numbers), "\t%d\t%d\t%.3f\t%.3f\t%d\n", result.readingLength, result.contextLength,
                  result.milliseconds, result.neuralNetworkMilliseconds, result.neuralNetworkCalls);
    return result.index + numbers;
}

std::string TimingsTsv(const std::vector<ItemResult>& results) {
    std::string tsv = kTimingsHeader;
    for (const ItemResult& result : results) tsv += TimingsLine(result);
    return tsv;
}

} // namespace iroha::benchmark
