// 変換エンジンの検証用CLIハーネス。
// 移植元: macos/Sources/iroha-cli/main.swift（bench出力の互換を維持し、
// macOS版と同じeval.tsvで数値を突き合わせられるようにする）
//
// 使い方:
//   iroha-cli kana <romaji>                       : ローマ字→かな変換のみ
//   iroha-cli convert [--context 文脈] [--n 候補数] <読み>
//   iroha-cli segment <読み>                       : 変換 + 文節分割の検証
//   iroha-cli bench <eval.tsv>                    : 評価（TSV: 読み\t正解）
//   iroha-cli repl                                : 対話モード
//   環境変数 IROHA_MODEL でモデルパスを上書き可能
//
// 注意: macOS版と違い学習・ユーザ辞書のデコレータは未移植（zenz単体）。
// 既定ストアが空ならmacOS版benchと同条件になる。

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "iroha/reading_aligner.h"
#include "iroha/romaji_composer.h"
#include "iroha/unicode.h"
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

std::string DefaultModelPath() {
    if (const wchar_t* env = _wgetenv(L"IROHA_MODEL")) {
        return Utf32ToUtf8(Utf16ToUtf32(env));
    }
    const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
    std::wstring base = localAppData ? localAppData : L".";
    return Utf32ToUtf8(
        Utf16ToUtf32(base + L"\\iroha\\models\\zenz-v3.1-small-Q5_K_M.gguf"));
}

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

bool ConvertOnce(iroha::ZenzEngine& engine, const std::u32string& reading,
                 const std::u32string& context, int count,
                 std::vector<std::u32string>* results) {
    std::string error;
    if (!engine.Convert(reading, context, count, results, &error)) {
        std::fprintf(stderr, "エラー: %s\n", error.c_str());
        return false;
    }
    return true;
}

void ConvertAndPrint(iroha::ZenzEngine& engine, const std::u32string& input,
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

int RunBench(iroha::ZenzEngine& engine, const std::wstring& path) {
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
        if (ConvertOnce(engine, reading, U"", 1, &candidates) && !candidates.empty()) {
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

    iroha::ZenzEngine engine(DefaultModelPath());

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
        return RunBench(engine, Utf32ToUtf16(args[2]));
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
