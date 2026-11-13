// 変換エンジンの検証用CLIハーネス。
// 移植元: macos/Sources/iroha-cli/main.swift（bench出力の互換を維持し、
// macOS版と同じeval.tsvで数値を突き合わせられるようにする）
//
// 使い方:
//   iroha-cli kana <romaji>                       : ローマ字→かな変換のみ
//   iroha-cli convert [--context 文脈] [--n 候補数] <読み>
//   iroha-cli segment <読み>                       : 変換 + 文節分割の検証
//   iroha-cli bench <eval.tsv> [候補数]            : 評価（TSV: 読み\t正解。候補数指定でn-best計測）
//   iroha-cli ajimee <evaluation_items.json>      : AJIMEE-Bench評価（acc@1・MinCER）
//   iroha-cli lattice <読み>                       : 辞書ラティスの生の候補（モデルを使わない）
//   iroha-cli lattice-dump <入力> <出力.jsonl> [--n 10] [--limit N] [--skip N]
//                                                 : 読みの一覧にラティスの読み一致候補を付けて書き出す
//   iroha-cli lattice-parity <macOS版の出力.jsonl> [--n 10] [--examples 10]
//                                                 : macOS 版の lattice-dump / ajimee-dump の候補と突き合わせる
//   iroha-cli remote <読み>                        : 変換サーバ経由の変換（IPC検証用）
//   iroha-cli repl                                : 対話モード
//   環境変数 IROHA_MODEL でモデルパスを上書き可能
//
// 注意: macOS版と違い学習・ユーザ辞書のデコレータは未移植（zenz単体）。
// 既定ストアが空ならmacOS版benchと同条件になる。

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ipc_protocol.h"
#include "iroha/json.h"
#include "iroha/lattice_converter.h"
#include "iroha/learning_engine.h"
#include "iroha/learning_store.h"
#include "iroha/reading_aligner.h"
#include "iroha/romaji_composer.h"
#include "iroha/unicode.h"
#include "iroha/user_dictionary_engine.h"
#include "iroha/user_dictionary_store.h"
#include "iroha/zenz_engine.h"

namespace {

using iroha::Utf16ToUtf32;
using iroha::Utf32ToUtf16;
using iroha::Utf32ToUtf8;
using iroha::Utf8ToUtf32;

void PrintUtf8(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
}

void Print32(const std::u32string& s) {
    PrintUtf8(Utf32ToUtf8(s));
}

std::wstring DataDir() {
    const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
    return (localAppData ? std::wstring(localAppData) : L".") + L"\\iroha";
}

std::string DefaultModelPath() {
    if (const wchar_t* env = _wgetenv(L"IROHA_MODEL")) {
        return Utf32ToUtf8(Utf16ToUtf32(env));
    }
    return Utf32ToUtf8(Utf16ToUtf32(DataDir() + L"\\models\\zenz-v3.1-small-Q5_K_M.gguf"));
}

std::filesystem::path StorePath(const wchar_t* envName, const wchar_t* fileName) {
    if (const wchar_t* env = _wgetenv(envName)) return std::filesystem::path(env);
    return std::filesystem::path(DataDir() + L"\\" + fileName);
}

// IME本体と同じ構成（zenz + ユーザ辞書 + 学習）でエンジンを組み立てる。
// IROHA_MODEL / IROHA_USER_DICT / IROHA_LEARNING で差し替えられる
struct EngineStack {
    iroha::ZenzEngine zenz{DefaultModelPath()};
    iroha::UserDictionaryStore userDictionary{
        StorePath(L"IROHA_USER_DICT", L"user-dictionary.json")};
    iroha::LearningStore learning{StorePath(L"IROHA_LEARNING", L"learning.json")};
    iroha::UserDictionaryEngine dictionaryEngine{
        &zenz, [this] { return userDictionary.Current(); }};
    iroha::LearningEngine engine{&dictionaryEngine,
                                 [this] { return learning.Current(); }};
};

std::u32string RomajiToKana(const std::u32string& input) {
    // ASCII文字を含む場合のみローマ字として解釈する
    const bool allAscii = std::all_of(input.begin(), input.end(),
                                      [](char32_t c) { return c < 0x80; });
    if (!allAscii) return input;
    iroha::RomajiComposer composer;
    composer.Input(input);
    composer.Flush();
    return composer.Display();
}

// レーベンシュタイン距離（CER算出用）
int EditDistance(const std::u32string& a, const std::u32string& b) {
    if (a.empty()) return static_cast<int>(b.size());
    if (b.empty()) return static_cast<int>(a.size());
    std::vector<int> previous(b.size() + 1);
    std::vector<int> current(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) previous[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        current[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); ++j) {
            const int substitution = previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, substitution});
        }
        std::swap(previous, current);
    }
    return previous[b.size()];
}

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start)
        .count();
}

bool ConvertOnce(iroha::ConversionEngine& engine, const std::u32string& reading,
                 const std::u32string& context, int count,
                 std::vector<std::u32string>* results) {
    std::string error;
    if (!engine.Convert(reading, context, count, results, &error)) {
        std::fprintf(stderr, "エラー: %s\n", error.c_str());
        return false;
    }
    return true;
}

void ConvertAndPrint(iroha::ConversionEngine& engine, const std::u32string& input,
                     const std::u32string& context, int count) {
    const std::u32string kana = RomajiToKana(input);
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::u32string> candidates;
    if (!ConvertOnce(engine, kana, context, count, &candidates)) return;
    const double ms = MillisecondsSince(start);
    std::u32string joined;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (i > 0) joined += U" / ";
        joined += candidates[i];
    }
    Print32(kana);
    PrintUtf8(" -> ");
    Print32(joined);
    std::printf("  [%.1fms]\n", ms);
}

int RunBench(iroha::ConversionEngine& engine, const std::wstring& path,
             int candidateCount) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "ファイルが読めません\n");
        return 1;
    }
    std::vector<std::pair<std::u32string, std::u32string>> pairs;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        pairs.emplace_back(Utf8ToUtf32(line.substr(0, tab)),
                           Utf8ToUtf32(line.substr(tab + 1)));
    }

    // ウォームアップ（モデルロードを計測から除外）
    {
        std::vector<std::u32string> ignored;
        ConvertOnce(engine, U"うぉーむあっぷ", U"", 1, &ignored);
    }

    int exactMatches = 0;
    int totalEditDistance = 0;
    int totalExpectedLength = 0;
    double totalMilliseconds = 0.0;
    for (const auto& [reading, expected] : pairs) {
        const auto start = std::chrono::steady_clock::now();
        std::vector<std::u32string> candidates;
        std::u32string result = reading;
        if (ConvertOnce(engine, reading, U"", candidateCount, &candidates) &&
            !candidates.empty()) {
            result = candidates.front();
        }
        totalMilliseconds += MillisecondsSince(start);
        totalEditDistance += EditDistance(result, expected);
        totalExpectedLength += static_cast<int>(expected.size());
        if (result == expected) {
            ++exactMatches;
        } else {
            PrintUtf8("  \xE2\x9C\x97 "); // ✗
            Print32(reading);
            PrintUtf8(" -> ");
            Print32(result);
            PrintUtf8(" （正解: ");
            Print32(expected);
            PrintUtf8("）\n");
        }
    }
    const double accuracy = 100.0 * exactMatches / pairs.size();
    const double cer = 100.0 * totalEditDistance / totalExpectedLength;
    std::printf("件数: %zu  完全一致: %d (%.1f%%)  CER: %.2f%%  平均: %.1fms/変換\n",
                pairs.size(), exactMatches, accuracy, cer,
                totalMilliseconds / pairs.size());
    return 0;
}

// AJIMEE-Bench (azooKey/AJIMEE-Bench) 評価。
// zenzaiと同じ方式: グリーディ変換1候補を許容解リストと照合し、
// acc@1（許容解のいずれかに完全一致）と MinCER（許容解との最小CERの平均）を報告する。
// 移植元: macos/Sources/iroha-cli/main.swift の ajimee コマンド
int RunAjimee(iroha::ConversionEngine& engine, const std::wstring& path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) {
        std::fprintf(stderr,
                     "JSONが読めません（windows\\scripts\\fetch-ajimee.ps1 で取得できます）\n");
        return 1;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    iroha::json::Value root;
    if (!iroha::json::Parse(buffer.str(), &root) ||
        root.type != iroha::json::Value::Type::Array) {
        std::fprintf(stderr, "JSONの解析に失敗しました\n");
        return 1;
    }

    struct Tally {
        int count = 0;
        int accAt1 = 0;
        double minCerSum = 0.0;
        std::string Summary() const {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "acc@1 %d/%d (%.1f%%)  MinCER %.2f%%",
                          accAt1, count, 100.0 * accAt1 / std::max(count, 1),
                          100.0 * minCerSum / std::max(count, 1));
            return buf;
        }
    };
    Tally withContext;
    Tally withoutContext;
    double totalMilliseconds = 0.0;

    // ウォームアップ（モデルロードを計測から除外）
    {
        std::vector<std::u32string> ignored;
        ConvertOnce(engine, U"うぉーむあっぷ", U"", 1, &ignored);
    }

    for (const iroha::json::Value& item : root.array) {
        const iroha::json::Value* indexValue = item.Find("index");
        const iroha::json::Value* contextValue = item.Find("context_text");
        const iroha::json::Value* inputValue = item.Find("input");
        const iroha::json::Value* expectedValue = item.Find("expected_output");
        if (!inputValue || !expectedValue ||
            expectedValue->type != iroha::json::Value::Type::Array) {
            continue;
        }
        const std::u32string input = Utf8ToUtf32(inputValue->AsString());
        const std::u32string context =
            contextValue ? Utf8ToUtf32(contextValue->AsString()) : U"";

        const auto start = std::chrono::steady_clock::now();
        std::vector<std::u32string> candidates;
        std::u32string result = input;
        if (ConvertOnce(engine, input, context, 1, &candidates) && !candidates.empty()) {
            result = candidates.front();
        }
        totalMilliseconds += MillisecondsSince(start);

        bool hit = false;
        double minCer = 1.0;
        for (const iroha::json::Value& reference : expectedValue->array) {
            const std::u32string expected = Utf8ToUtf32(reference.AsString());
            if (result == expected) hit = true;
            const double cer =
                static_cast<double>(EditDistance(result, expected)) /
                std::max<size_t>(expected.size(), 1);
            minCer = std::min(minCer, cer);
        }
        Tally& tally = context.empty() ? withoutContext : withContext;
        ++tally.count;
        if (hit) ++tally.accAt1;
        tally.minCerSum += minCer;

        if (!hit) {
            PrintUtf8("  \xE2\x9C\x97 [");
            PrintUtf8(indexValue ? indexValue->AsString() : "?");
            PrintUtf8("] ");
            Print32(input);
            PrintUtf8(" -> ");
            Print32(result);
            PrintUtf8("\n");
        }
    }

    if (withoutContext.count > 0) {
        std::printf("文脈なし: %s\n", withoutContext.Summary().c_str());
    }
    if (withContext.count > 0) {
        std::printf("文脈あり: %s\n", withContext.Summary().c_str());
    }
    Tally total;
    total.count = withContext.count + withoutContext.count;
    total.accAt1 = withContext.accAt1 + withoutContext.accAt1;
    total.minCerSum = withContext.minCerSum + withoutContext.minCerSum;
    std::printf("全体: %s  平均 %.1fms/変換\n", total.Summary().c_str(),
                totalMilliseconds / std::max(total.count, 1));
    return 0;
}

// 辞書フォルダを探してラティスを作る（見つからなければ nullptr）
std::unique_ptr<iroha::LatticeConverter> MakeLattice() {
    std::filesystem::path executableDirectory;
    wchar_t modulePath[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        executableDirectory = std::filesystem::path(modulePath).parent_path();
    }
    const auto dictionary = iroha::LatticeConverter::DefaultDictionaryPath(executableDirectory);
    if (!dictionary) {
        std::fprintf(stderr,
                     "辞書が見つかりません（windows\\scripts\\fetch-dictionary.ps1 で取得できます）\n");
        return nullptr;
    }
    return std::make_unique<iroha::LatticeConverter>(*dictionary);
}

// 辞書ラティスの生の候補を見る（調査用）。★は読み全体に一致した候補。
// 移植元: macos/Sources/iroha-cli/main.swift の lattice コマンド（モデルの採点の列は出さない）
int RunLattice(const std::u32string& reading) {
    const auto lattice = MakeLattice();
    if (!lattice) return 1;
    const auto start = std::chrono::steady_clock::now();
    const auto candidates = lattice->RawCandidates(reading, 20);
    const double ms = MillisecondsSince(start);
    Print32(reading);
    std::printf("  [%.0fms]\n", ms);
    for (const auto& candidate : candidates) {
        PrintUtf8(candidate.isFullMatch ? "  \xE2\x98\x85 " : "  \xE3\x80\x80 "); // ★ / 全角スペース
        std::printf("%8.2f           ", candidate.value);
        Print32(candidate.text);
        PrintUtf8("\n");
    }
    return 0;
}

// 読みの一覧に、辞書ラティスの読み一致候補を付けて JSONL に書く（モデルは使わない）。
// 移植元: macos/Sources/iroha-cli/main.swift の lattice-dump コマンド。macOS 版と同じ入力で
// 書き出して突き合わせれば、ラティスの移植が合っているかを確かめられる。
// 入力の行形式は 2 種類を自動判別する:
//   1. prepare_data.py の出力: [U+EE02 文脈]U+EE00 読み U+EE01 正解
//   2. TSV: 読み\t正解[\t文脈]
// 出力の 1 行: {"context","reading","gold","lattice":[読み一致候補（ラティス順・全件）],"goldRank":正解の位置(-1=なし),"ms"}
int RunLatticeDump(const std::wstring& inputPath, const std::wstring& outputPath, int nBest,
                   int limit, int skip) {
    const auto lattice = MakeLattice();
    if (!lattice) return 1;
    std::ifstream input(std::filesystem::path(inputPath), std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "入力が読めません\n");
        return 1;
    }
    std::ofstream output(std::filesystem::path(outputPath), std::ios::binary);
    if (!output) {
        std::fprintf(stderr, "出力が開けません\n");
        return 1;
    }
    lattice->Candidates(U"うぉーむあっぷ", 1);

    int lineNumber = 0;
    int written = 0;
    int skippedMalformed = 0;
    int goldInLattice = 0;
    int goldFirst = 0;
    double totalMilliseconds = 0.0;
    std::string line;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (lineNumber <= skip) continue;
        if (written >= limit) break;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::u32string context, reading, gold;
        const std::u32string text = Utf8ToUtf32(line);
        const size_t readingStart = text.find(U'');
        if (readingStart != std::u32string::npos) {
            const size_t outputStart = text.find(U'', readingStart + 1);
            if (outputStart == std::u32string::npos) {
                ++skippedMalformed;
                continue;
            }
            context = text.substr(0, readingStart);
            if (!context.empty() && context[0] == U'') context.erase(0, 1);
            reading = text.substr(readingStart + 1, outputStart - readingStart - 1);
            gold = text.substr(outputStart + 1);
        } else {
            const size_t firstTab = text.find(U'\t');
            if (firstTab != std::u32string::npos) {
                const size_t secondTab = text.find(U'\t', firstTab + 1);
                reading = text.substr(0, firstTab);
                gold = text.substr(firstTab + 1, secondTab == std::u32string::npos
                                                     ? std::u32string::npos
                                                     : secondTab - firstTab - 1);
                if (secondTab != std::u32string::npos) {
                    const size_t thirdTab = text.find(U'\t', secondTab + 1);
                    context = text.substr(secondTab + 1, thirdTab == std::u32string::npos
                                                             ? std::u32string::npos
                                                             : thirdTab - secondTab - 1);
                }
            }
        }
        if (reading.empty() || gold.empty()) {
            ++skippedMalformed;
            continue;
        }

        const auto start = std::chrono::steady_clock::now();
        const std::vector<std::u32string> candidates = lattice->FullMatchCandidates(reading, nBest);
        const double ms = MillisecondsSince(start);
        totalMilliseconds += ms;
        const auto goldPosition = std::find(candidates.begin(), candidates.end(), gold);
        const int goldRank =
            goldPosition == candidates.end() ? -1 : static_cast<int>(goldPosition - candidates.begin());
        if (goldRank >= 0) ++goldInLattice;
        if (goldRank == 0) ++goldFirst;

        std::string json = "{\"context\":" + iroha::json::QuoteString(Utf32ToUtf8(context)) +
                           ",\"reading\":" + iroha::json::QuoteString(Utf32ToUtf8(reading)) +
                           ",\"gold\":" + iroha::json::QuoteString(Utf32ToUtf8(gold)) +
                           ",\"lattice\":[";
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (i > 0) json += ",";
            json += iroha::json::QuoteString(Utf32ToUtf8(candidates[i]));
        }
        char tail[64];
        std::snprintf(tail, sizeof(tail), "],\"goldRank\":%d,\"ms\":%.3f}\n", goldRank, ms);
        json += tail;
        output.write(json.data(), static_cast<std::streamsize>(json.size()));
        ++written;
        if (written % 1000 == 0) {
            std::fprintf(stderr, "%d 件  ラティス平均 %.1fms\n", written, totalMilliseconds / written);
        }
    }
    const int denominator = std::max(written, 1);
    std::fprintf(stderr,
                 "%d 件を書き出しました（形式不正 %d 行）。ラティス平均 %.1fms、"
                 "正解がラティス内 %d (%.1f%%)、ラティス1位が正解 %d (%.1f%%)\n",
                 written, skippedMalformed, totalMilliseconds / denominator, goldInLattice,
                 100.0 * goldInLattice / denominator, goldFirst, 100.0 * goldFirst / denominator);
    return 0;
}

// macOS 版が書き出した JSONL（iroha-cli lattice-dump / ajimee-dump。1 行に "reading" と "lattice"）の
// 読みを同じ条件で変換し直し、候補の並びが macOS 版と一致するかを数える。
// ラティスの移植が正しいかの判定に使う（`--n` は書き出したときの候補数に合わせる）
int RunLatticeParity(const std::wstring& referencePath, int nBest, int examples) {
    const auto lattice = MakeLattice();
    if (!lattice) return 1;
    std::ifstream input(std::filesystem::path(referencePath), std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "入力が読めません\n");
        return 1;
    }
    int total = 0;
    int exact = 0;        // 候補の並びがすべて同じ
    int sameSet = 0;      // 並びは違うが候補の集合は同じ
    int sameFirst = 0;    // 第一候補が同じ
    int shown = 0;
    double totalMilliseconds = 0.0;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        iroha::json::Value record;
        if (line.empty() || !iroha::json::Parse(line, &record)) continue;
        const iroha::json::Value* readingValue = record.Find("reading");
        const iroha::json::Value* latticeValue = record.Find("lattice");
        if (!readingValue || !latticeValue ||
            latticeValue->type != iroha::json::Value::Type::Array) {
            continue;
        }
        const std::u32string reading = Utf8ToUtf32(readingValue->AsString());
        std::vector<std::u32string> expected;
        for (const iroha::json::Value& item : latticeValue->array) {
            expected.push_back(Utf8ToUtf32(item.AsString()));
        }

        const auto start = std::chrono::steady_clock::now();
        const std::vector<std::u32string> actual = lattice->FullMatchCandidates(reading, nBest);
        totalMilliseconds += MillisecondsSince(start);
        ++total;
        if (!actual.empty() && !expected.empty() && actual.front() == expected.front()) ++sameFirst;
        if (actual == expected) {
            ++exact;
            continue;
        }
        std::vector<std::u32string> sortedActual = actual;
        std::vector<std::u32string> sortedExpected = expected;
        std::sort(sortedActual.begin(), sortedActual.end());
        std::sort(sortedExpected.begin(), sortedExpected.end());
        if (sortedActual == sortedExpected) ++sameSet;
        if (shown++ >= examples) continue;
        PrintUtf8("  \xE2\x9C\x97 "); // ✗
        Print32(reading);
        PrintUtf8(sortedActual == sortedExpected ? "  （並びだけ違う）\n" : "\n");
        const size_t rows = std::max(actual.size(), expected.size());
        for (size_t i = 0; i < rows; ++i) {
            const std::u32string left = i < expected.size() ? expected[i] : U"";
            const std::u32string right = i < actual.size() ? actual[i] : U"";
            if (left == right) continue;
            std::printf("      [%zu] macOS: ", i);
            Print32(left);
            PrintUtf8("\n          Windows: ");
            Print32(right);
            PrintUtf8("\n");
        }
    }
    const int denominator = std::max(total, 1);
    std::printf("件数: %d  並びまで一致: %d (%.1f%%)  集合は一致: %d  第一候補が一致: %d (%.1f%%)  平均 %.1fms\n",
                total, exact, 100.0 * exact / denominator, sameSet, sameFirst,
                100.0 * sameFirst / denominator, totalMilliseconds / denominator);
    return exact == total ? 0 : 2;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::u32string> args;
    for (int i = 0; i < argc; ++i) args.push_back(Utf16ToUtf32(argv[i]));

    const std::u32string command = args.size() > 1 ? args[1] : U"repl";

    if (command == U"kana" && args.size() >= 3) {
        Print32(RomajiToKana(args[2]));
        PrintUtf8("\n");
        return 0;
    }

    if (command == U"remote" && args.size() >= 3) {
        // 変換サーバ経由（TIPと同じ経路）。サーバは別途起動しておくこと
        const std::u32string kana = RomajiToKana(args[2]);
        const auto start = std::chrono::steady_clock::now();
        std::vector<char> responseBytes;
        if (!iroha::ipc::Call(
                iroha::ipc::BuildConvertRequest(Utf32ToUtf8(kana), "", 9),
                &responseBytes, 15000)) {
            std::fprintf(stderr, "サーバに接続できません（iroha-server.exe を起動してください）\n");
            return 1;
        }
        const auto response = iroha::ipc::ParseConvertResponse(responseBytes);
        if (!response.ok) {
            std::fprintf(stderr, "サーバエラー: %s\n", response.error.c_str());
            return 1;
        }
        const double ms = MillisecondsSince(start);
        Print32(kana);
        PrintUtf8(" -> ");
        for (size_t i = 0; i < response.candidates.size(); ++i) {
            if (i > 0) PrintUtf8(" / ");
            PrintUtf8(response.candidates[i]);
        }
        std::printf("  [%.1fms]\n", ms);
        return 0;
    }

    if (command == U"lattice" && args.size() >= 3) {
        return RunLattice(RomajiToKana(args[2]));
    }

    if (command == U"lattice-parity" && args.size() >= 3) {
        int nBest = 10;
        int examples = 10;
        for (size_t index = 3; index + 1 < args.size(); index += 2) {
            const int value = std::atoi(Utf32ToUtf8(args[index + 1]).c_str());
            if (args[index] == U"--n") nBest = value > 0 ? value : 10;
            if (args[index] == U"--examples") examples = value;
        }
        return RunLatticeParity(Utf32ToUtf16(args[2]), nBest, examples);
    }

    if (command == U"lattice-dump" && args.size() >= 4) {
        int nBest = 10;
        int limit = INT_MAX;
        int skip = 0;
        for (size_t index = 4; index < args.size(); index += 2) {
            if (index + 1 >= args.size()) {
                std::fprintf(stderr, "不明な引数: %s\n", Utf32ToUtf8(args[index]).c_str());
                return 1;
            }
            const int value = std::atoi(Utf32ToUtf8(args[index + 1]).c_str());
            if (args[index] == U"--n") {
                nBest = value > 0 ? value : 10;
            } else if (args[index] == U"--limit") {
                limit = value;
            } else if (args[index] == U"--skip") {
                skip = value;
            } else {
                std::fprintf(stderr, "不明な引数: %s\n", Utf32ToUtf8(args[index]).c_str());
                return 1;
            }
        }
        return RunLatticeDump(Utf32ToUtf16(args[2]), Utf32ToUtf16(args[3]), nBest, limit, skip);
    }

    EngineStack stack;
    iroha::ConversionEngine& engine = stack.engine;

    if (command == U"convert") {
        std::u32string context;
        int count = 1;
        std::u32string reading;
        for (size_t index = 2; index < args.size();) {
            if (args[index] == U"--context" && index + 1 < args.size()) {
                context = args[index + 1];
                index += 2;
            } else if (args[index] == U"--n" && index + 1 < args.size()) {
                count = std::max(1, std::atoi(Utf32ToUtf8(args[index + 1]).c_str()));
                index += 2;
            } else {
                reading = args[index];
                ++index;
            }
        }
        if (reading.empty()) {
            std::fprintf(stderr,
                         "使い方: iroha-cli convert [--context 文脈] [--n 候補数] <読み>\n");
            return 1;
        }
        ConvertAndPrint(engine, reading, context, count);
        return 0;
    }

    if (command == U"segment" && args.size() >= 3) {
        const std::u32string kana = RomajiToKana(args[2]);
        std::vector<std::u32string> candidates;
        if (!ConvertOnce(engine, kana, U"", 1, &candidates)) return 1;
        const std::u32string conversion = candidates.empty() ? kana : candidates.front();
        const auto segments = iroha::ReadingAligner::SegmentReading(kana, conversion);
        std::u32string conversions, readings;
        for (size_t i = 0; i < segments.size(); ++i) {
            if (i > 0) {
                conversions += U"|";
                readings += U"|";
            }
            conversions += segments[i].conversion;
            readings += segments[i].reading;
        }
        Print32(kana);
        PrintUtf8(" -> ");
        Print32(conversions);
        PrintUtf8("  (");
        Print32(readings);
        PrintUtf8(")\n");
        return 0;
    }

    if (command == U"bench" && args.size() >= 3) {
        int candidateCount = 1;
        if (args.size() >= 4) {
            candidateCount = std::max(1, std::atoi(Utf32ToUtf8(args[3]).c_str()));
        }
        return RunBench(engine, Utf32ToUtf16(args[2]), candidateCount);
    }

    if (command == U"ajimee" && args.size() >= 3) {
        return RunAjimee(engine, Utf32ToUtf16(args[2]));
    }

    // repl
    std::fprintf(stderr, "読みを入力してください（Ctrl-Zで終了）\n");
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) break;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ConvertAndPrint(engine, Utf8ToUtf32(line), U"", 1);
    }
    return 0;
}
