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
//   iroha-cli model-parity <macOS版のajimee-dumpの出力.jsonl> [--examples 10]
//                                                 : モデルの生成と採点を macOS 版の出力と突き合わせる
//   iroha-cli model-selfcheck <同じ形式.jsonl> [--limit N]
//                                                 : モデルの採点が生成と辻褄が合うかを確かめる（macOS版の出力は使わない）
//   iroha-cli remote <読み>                        : 変換サーバ経由の変換（IPC検証用）
//   iroha-cli confidence [--context 文脈] <読み>    : モデル単体の貪欲変換と文字ごとの自信度（AIで直すの判定に使う値）
//   iroha-cli terms list | install <ID> | remove <ID> | lookup <読み> <候補>...
//                                                 : 専門辞書の一覧・取得と設置・削除（IME本体と同じ経路）・辞書名の印を引く
//   iroha-cli symbols [読み...]                     : 記号・顔文字・絵文字の表（Mozc のデータ）を引く。
//                                                   Windows で描けないとして除いた絵文字も数える
//   iroha-cli typo [--threshold θ] [--min-length N] <読み>...
//                                                 : 打ち間違いの訂正（読みはかなのまま渡す。生成・margin・訂正・時間）
//   iroha-cli typo catalog [install [ID]]         : 訂正モデルの一覧・取得と設置（IME本体と同じ経路。ID が無ければ先頭）
//   iroha-cli typo logits <入力の読み> <正しい読み> [ステップ数]
//                                                 : teacher forcing のロジットを JSON で出す（照合用）
//   iroha-cli ai [--preset 0〜2] <テキスト>        : AI変換を 1 回依頼する（設定画面の接続先・プリセット。途中経過も出す）
//   iroha-cli ai models                           : 設定画面の接続先で使えるモデルの一覧
//   iroha-cli typo parity [--dir DIR]             : 書き出しに付いてくる parity.json と突き合わせる（macOS版と同じ判定）
//                                                   モデルの場所は IROHA_TYPO_MODEL か <データフォルダ>/models/typo-normalizer
//   iroha-cli session [--context 文脈] [--interval ミリ秒] <キー列>
//                                                 : 変換サーバのセッションにキーを順に送り、TIP が画面に出す内容を見る。
//                                                   キー列は打つ文字をそのまま並べ、特別なキーは <space> <enter> <esc>
//                                                   <bs> <left> <right> <up> <down> <f6>〜<f10> <punct>（Ctrl+.）
//                                                   <undo>（確定の取り消し）（Shift は <S-left> のように）と書く。
//                                                   <wait> は、キーを送らずに変換・打ち間違いの訂正が届くまで待つ。
//                                                   <ai0>〜<ai2> は AI変換（プリセットの番号）、<fix> は AIで直す。
//                                                   <vstart> <vstop> <vcancel> は「こえ」モードのキーを押す・離す・ほかのキー、
//                                                   <pause1500> はキーを送らずにその時間（ミリ秒）だけ届いた表示を出す
//                                                   （変換サーバを IROHA_VOICE_FILE=<WAV> で起動すると、マイクの代わりにそれを流す）。
//                                                   既定ではキーごとにライブ変換の到着を待つ。--interval を付けると
//                                                   待たずにその間隔で送る（変換が打鍵に追いつかないときの表示を見る）
//   iroha-cli voice [--stream] [--say 文] <音声.wav>...
//                                                 : 「こえ」モードの認識（SAPI の日本語の認識エンジン）。途中結果と届いた時刻・
//                                                   語ごとの表記と読み・読みに戻した結果を出す。--stream はマイクと同じく
//                                                   実時間の速さで流す。--say は先に日本語の音声合成で文を読み上げて WAV に書く
//   iroha-cli voice --mic <秒>                    : マイクから決めた秒数だけ録音して認識する（変換サーバと同じ経路）
//   iroha-cli repl                                : 対話モード
//   環境変数 IROHA_MODEL でモデルパス、IROHA_LORA で追加学習した LoRA アダプタ（GGUF）、
//   IROHA_USER_DICT でユーザ辞書、IROHA_LEARNING で学習結果のファイルを上書き可能。
//   bench / ajimee はモデルの素の力を測るため、既定でユーザ辞書・学習を空にする
//   （IROHA_WITH_USER_DATA=1 でIME本体のデータを使う。IROHA_USER_DICT / IROHA_LEARNING の
//   明示指定はそのまま使う）。convert / segment / repl はIME本体と同じデータを使う
//   IROHA_LATTICE=off で辞書ラティスを使わずモデル単体、IROHA_LATTICE=always で第一候補も
//   ラティス候補の再採点で決める（既定は候補数 2 以上のときだけラティス。IME本体と同じ）
//   IROHA_TERM_DICTIONARIES=installed で IME 本体に入れた専門辞書を、=a.tsv,b.tsv でその辞書ファイルを
//   候補ウィンドウの辞書ラティスに加える（既定は使わない。macOS版と同じ）
//   IROHA_NO_LATIN=1 で読みにラテン文字がないときの英字出力を禁じる（実験用）
//   IROHA_NO_CONSTRAINT=1 で読み制約（constrained decoding）を丸ごと切り素の貪欲生成にする（実験用）
//   IROHA_KV_REUSE=0 で前回の計算の再利用（KVキャッシュ・エンコーダ出力）を切る（切り分け用）
//   IROHA_THREADS で推論のスレッド数を変える（計測用）
//   IROHA_TIMINGS=<path> で ajimee の項目ごとの変換時間を TSV に書く（手順と集計は testdata/speed/README.md）

#define NOMINMAX
#include <windows.h>
#include <sapi.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "emoji_renderability.h"
#include "ipc_protocol.h"
#include "iroha/benchmark.h"
#include "iroha/chunked_conversion_engine.h"
#include "iroha/config.h"
#include "iroha/engine_assembly.h"
#include "iroha/inference_timer.h"
#include "iroha/json.h"
#include "iroha/lattice_converter.h"
#include "iroha/lattice_rescoring_engine.h"
#include "iroha/learning_engine.h"
#include "iroha/learning_store.h"
#include "iroha/reading_aligner.h"
#include "iroha/romaji_composer.h"
#include "iroha/symbol_candidates.h"
#include "iroha/term_dictionary.h"
#include "iroha/typo_normalizer.h"
#include "iroha/typo_normalizer_catalog.h"
#include "term_dictionary_fetcher.h"
#include "ai_client.h"
#include "secret_store.h"
#include "typo_normalizer_fetcher.h"
#include "voice_recognizer.h"
#include "iroha/unicode.h"
#include "iroha/user_dictionary_engine.h"
#include "iroha/user_dictionary_store.h"
#include "iroha/variant_kanji_engine.h"
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
    // IME本体と同じ既定のモデル（iroha-kkc-v1。未取得で以前の既定があればそれ）
    return Utf32ToUtf8(Utf16ToUtf32(
        iroha::DefaultModelPath(std::filesystem::path(DataDir() + L"\\models")).wstring()));
}

std::filesystem::path StorePath(const wchar_t* envName, const wchar_t* fileName) {
    if (const wchar_t* env = _wgetenv(envName)) return std::filesystem::path(env);
    return std::filesystem::path(DataDir() + L"\\" + fileName);
}

bool EnvEquals(const wchar_t* name, const wchar_t* value) {
    const wchar_t* env = _wgetenv(name);
    return env && std::wstring(env) == value;
}

// IROHA_LORA / IROHA_NO_LATIN / IROHA_NO_CONSTRAINT を反映したモデルの設定（macOS版 iroha-cli と同じ）
iroha::ZenzEngine::Options ZenzOptions() {
    iroha::ZenzEngine::Options options;
    if (const wchar_t* adapter = _wgetenv(L"IROHA_LORA")) {
        options.adapterPath = Utf32ToUtf8(Utf16ToUtf32(adapter));
    }
    // IROHA_NO_LATIN=1: 読みにラテン文字がなければ出力の英字を禁じる（英語語彙の多いモデルの評価用）
    options.restrictLatinToReading = EnvEquals(L"IROHA_NO_LATIN", L"1");
    // IROHA_NO_CONSTRAINT=1: 読み制約を切って素の貪欲生成にする（制約なし計測との突き合わせ用）
    options.usesReadingConstraint = !EnvEquals(L"IROHA_NO_CONSTRAINT", L"1");
    if (!options.usesReadingConstraint) {
        std::fprintf(stderr, "読み制約なし（IROHA_NO_CONSTRAINT=1）\n");
    }
    return options;
}

std::filesystem::path ExecutableDirectory() {
    wchar_t modulePath[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(modulePath).parent_path();
}

// 専門辞書（IROHA_TERM_DICTIONARIES）。`installed` なら IME 本体で入れた辞書、それ以外はカンマ区切りの
// 辞書ファイル（読み<TAB>語、用語として扱う）。指定が無ければ使わない（IME 本体と違い、既定は無し）
std::shared_ptr<const iroha::TermLatticeIndex> CliTermIndex(const std::filesystem::path& dictionaryPath) {
    const wchar_t* env = _wgetenv(L"IROHA_TERM_DICTIONARIES");
    if (!env || env[0] == L'\0') return nullptr;
    const std::wstring value = env;
    std::vector<iroha::TermDictionaryPack> packs;
    if (value == L"installed") {
        const std::filesystem::path data = DataDir();
        packs = iroha::TermDictionaryInstall::LoadPacks(
            data, iroha::TermDictionaryInstall::InstalledRecords(data));
    } else {
        size_t start = 0;
        while (start <= value.size()) {
            size_t end = value.find(L',', start);
            if (end == std::wstring::npos) end = value.size();
            std::ifstream file(std::filesystem::path(value.substr(start, end - start)), std::ios::binary);
            std::stringstream buffer;
            buffer << file.rdbuf();
            packs.push_back({iroha::TermKind::kNoun, iroha::TermDictionary::Parse(buffer.str())});
            start = end + 1;
        }
    }
    const auto started = std::chrono::steady_clock::now();
    auto index = iroha::LatticeConverter::BuildTermIndex(dictionaryPath, packs);
    std::fprintf(stderr, "専門辞書: %zu 語・%.0fms（IROHA_TERM_DICTIONARIES）\n",
                 iroha::LatticeConverter::TermIndexEntryCount(*index),
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                     .count());
    return index;
}

// ユーザ辞書・学習の扱い。評価（bench / ajimee）はモデル単体の力を測るので既定で空にする。
// 実データを混ぜると学習ファイルの成長で同じモデルでも数値が変わり、再現性がなくなる
enum class UserDataMode {
    // IME本体と同じファイル（IROHA_USER_DICT / IROHA_LEARNING で差し替え可）
    Ime,
    // 空。IROHA_WITH_USER_DATA=1 なら Ime と同じ、IROHA_USER_DICT / IROHA_LEARNING の明示指定は使う
    Evaluation,
};

// 存在しないファイルを指すストアは空として振る舞う（評価では書き込みも起きない）
std::filesystem::path UserDataPath(UserDataMode mode, const wchar_t* envName,
                                   const wchar_t* fileName) {
    if (_wgetenv(envName) || mode == UserDataMode::Ime ||
        EnvEquals(L"IROHA_WITH_USER_DATA", L"1")) {
        return StorePath(envName, fileName);
    }
    return std::filesystem::temp_directory_path() /
           (L"iroha-cli-empty-" + std::to_wstring(GetCurrentProcessId())) / fileName;
}

// IME本体と同じ構成（iroha::EngineAssembly。変換サーバ・iroha-bench と同じもの）でエンジンを組み立てる。
// IROHA_MODEL / IROHA_USER_DICT / IROHA_LEARNING で差し替えられる。
// IROHA_LATTICE=off で辞書ラティスを使わずモデル単体、IROHA_LATTICE=always で第一候補も
// ラティス候補の再採点で決める（既定は候補ウィンドウのみラティス）
struct EngineStack {
    explicit EngineStack(UserDataMode mode = UserDataMode::Ime)
        : zenz(DefaultModelPath(), ZenzOptions()),
          lattice(MakeLatticeForStack()),
          userDictionary(UserDataPath(mode, L"IROHA_USER_DICT", L"user-dictionary.json")),
          learning(UserDataPath(mode, L"IROHA_LEARNING", L"learning.json")),
          assembly(&zenz, &zenz, lattice.get(), LatticeUseFromEnvironment(),
                   [this] { return userDictionary.Current(); }, [this] { return learning.Current(); }),
          engine(assembly.Engine()) {
        if (mode == UserDataMode::Evaluation) {
            std::fprintf(stderr, "%s\n",
                         EnvEquals(L"IROHA_WITH_USER_DATA", L"1")
                             ? "IME本体のユーザ辞書・学習を使用（IROHA_WITH_USER_DATA=1）"
                             : "ユーザ辞書・学習は空（IME本体のデータを使うなら IROHA_WITH_USER_DATA=1）");
        }
    }

    iroha::ZenzEngine zenz;
    std::unique_ptr<iroha::LatticeConverter> lattice;
    iroha::UserDictionaryStore userDictionary;
    iroha::LearningStore learning;
    iroha::EngineAssembly assembly;
    iroha::LearningEngine& engine;

private:
    static iroha::LatticeUse LatticeUseFromEnvironment() {
        if (EnvEquals(L"IROHA_LATTICE", L"off")) return iroha::LatticeUse::Off;
        if (EnvEquals(L"IROHA_LATTICE", L"always")) return iroha::LatticeUse::FirstCandidate;
        return iroha::LatticeUse::Candidates;
    }
    // 辞書ラティス（辞書が無ければ nullptr でモデル単体。IROHA_LATTICE=off のときもユーザ辞書の語の境目の判定に使う）
    static std::unique_ptr<iroha::LatticeConverter> MakeLatticeForStack() {
        const auto dictionary = iroha::LatticeConverter::DefaultDictionaryPath(ExecutableDirectory());
        if (!dictionary) return nullptr;
        auto lattice = std::make_unique<iroha::LatticeConverter>(*dictionary);
        lattice->SetTermDictionary(CliTermIndex(*dictionary));
        return lattice;
    }
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

// レーベンシュタイン距離（CER算出用。ベンチマークの採点と同じもの）
using iroha::benchmark::EditDistance;

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
    // 読み込み・採点・集計はアプリ内のベンチマーク（iroha-bench）と同じ実装（iroha/benchmark.h）。
    // AJIMEE の JSON の配列のほか、配布用の評価セット（JSON Lines）も読める
    if (!std::filesystem::exists(std::filesystem::path(path))) {
        std::fprintf(stderr,
                     "JSONが読めません（windows\\scripts\\fetch-ajimee.ps1 で取得できます）\n");
        return 1;
    }
    std::vector<iroha::benchmark::Item> items;
    std::string loadError;
    if (!iroha::benchmark::LoadItems(std::filesystem::path(path), &items, &loadError)) {
        std::fprintf(stderr, "%s\n", loadError.c_str());
        return 1;
    }
    // IROHA_TIMINGS=<path> で項目ごとの変換時間を TSV に書く（macOS版と同じ指定で、先頭の 4 列も同じ。
    // 集計は testdata/speed/summarize.py）。nn_ms・nn_calls は NN の計算の合計時間と回数（inference_timer）
    const char* timingsPath = std::getenv("IROHA_TIMINGS");

    // ウォームアップ（モデルロードを計測から除外）
    iroha::benchmark::WarmUp(engine);

    std::vector<iroha::benchmark::ItemResult> results;
    results.reserve(items.size());
    for (const iroha::benchmark::Item& item : items) {
        std::string error;
        iroha::benchmark::ItemResult result = iroha::benchmark::Run(item, engine, &error);
        if (!error.empty()) std::fprintf(stderr, "エラー: %s\n", error.c_str());
        if (!result.hit) {
            PrintUtf8("  \xE2\x9C\x97 [");
            PrintUtf8(item.index);
            PrintUtf8("] ");
            Print32(item.input);
            PrintUtf8(" -> ");
            Print32(result.output);
            PrintUtf8("\n");
        }
        results.push_back(std::move(result));
    }

    const iroha::benchmark::Summary summary = iroha::benchmark::Summary::Of(results);
    if (summary.withoutContext.count > 0) {
        std::printf("文脈なし: %s\n", summary.withoutContext.SummaryText().c_str());
    }
    if (summary.withContext.count > 0) {
        std::printf("文脈あり: %s\n", summary.withContext.SummaryText().c_str());
    }
    std::printf("全体: %s  平均 %.1fms/変換\n", summary.total.SummaryText().c_str(), summary.latency.mean);
    if (timingsPath) {
        std::ofstream out(std::filesystem::path(timingsPath), std::ios::binary);
        out << iroha::benchmark::TimingsTsv(results);
    }
    return 0;
}

// 辞書フォルダを探してラティスを作る（見つからなければ nullptr）
std::unique_ptr<iroha::LatticeConverter> MakeLattice() {
    const auto dictionary = iroha::LatticeConverter::DefaultDictionaryPath(ExecutableDirectory());
    if (!dictionary) {
        std::fprintf(stderr,
                     "辞書が見つかりません（windows\\scripts\\fetch-dictionary.ps1 で取得できます）\n");
        return nullptr;
    }
    auto lattice = std::make_unique<iroha::LatticeConverter>(*dictionary);
    lattice->SetTermDictionary(CliTermIndex(*dictionary));
    return lattice;
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

// macOS 版の `iroha-cli ajimee-dump` の出力（1 行に reading・context・generated・scoredCandidates・zenzScores）と、
// モデルの生成（第一候補）と採点（候補ごとの対数確率）を突き合わせる。モデルの移植が正しいかの判定に使う。
// 同じモデル（IROHA_MODEL）で比べること。計算機が違う（Metal と CPU）ので、採点は誤差の範囲で一致すればよい
int RunModelParity(const std::wstring& referencePath, int examples) {
    std::ifstream input(std::filesystem::path(referencePath), std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "入力が読めません\n");
        return 1;
    }
    iroha::ZenzEngine zenz(DefaultModelPath(), ZenzOptions());
    std::string error;
    if (!zenz.Prewarm(&error)) {
        std::fprintf(stderr, "エラー: %s\n", error.c_str());
        return 1;
    }
    constexpr double kTolerance = 0.05;
    int total = 0;
    int sameGenerated = 0;
    int scoredItems = 0;     // 採点を比べた項目
    int sameBest = 0;        // 採点が最も高い候補が同じ項目
    int scoreCount = 0;      // 比べた候補の数
    int scoreWithin = 0;     // 差が kTolerance 以内の候補の数
    double maxDifference = 0.0;
    double generateMilliseconds = 0.0;
    double scoreMilliseconds = 0.0;
    int shown = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        iroha::json::Value record;
        if (line.empty() || !iroha::json::Parse(line, &record)) continue;
        const iroha::json::Value* readingValue = record.Find("reading");
        const iroha::json::Value* generatedValue = record.Find("generated");
        if (!readingValue || !generatedValue) continue;
        const iroha::json::Value* contextValue = record.Find("context");
        const std::u32string reading = Utf8ToUtf32(readingValue->AsString());
        const std::u32string context = contextValue ? Utf8ToUtf32(contextValue->AsString()) : U"";
        const std::u32string expected = Utf8ToUtf32(generatedValue->AsString());

        auto start = std::chrono::steady_clock::now();
        std::vector<std::u32string> converted;
        if (!zenz.Convert(reading, context, 1, &converted, &error)) {
            std::fprintf(stderr, "エラー: %s\n", error.c_str());
            return 1;
        }
        generateMilliseconds += MillisecondsSince(start);
        ++total;
        const std::u32string actual = converted.empty() ? U"" : converted.front();
        if (actual == expected) {
            ++sameGenerated;
        } else if (shown++ < examples) {
            PrintUtf8("  \xE2\x9C\x97 生成 macOS: ");
            Print32(expected);
            PrintUtf8("\n         Windows: ");
            Print32(actual);
            PrintUtf8("\n");
        }

        const iroha::json::Value* candidatesValue = record.Find("scoredCandidates");
        const iroha::json::Value* scoresValue = record.Find("zenzScores");
        if (!candidatesValue || !scoresValue ||
            candidatesValue->array.size() != scoresValue->array.size() ||
            candidatesValue->array.empty()) {
            continue;
        }
        std::vector<std::u32string> candidates;
        for (const iroha::json::Value& item : candidatesValue->array) {
            candidates.push_back(Utf8ToUtf32(item.AsString()));
        }
        start = std::chrono::steady_clock::now();
        std::vector<float> scores;
        if (!zenz.Score(candidates, reading, context, &scores, &error)) {
            std::fprintf(stderr, "エラー: %s\n", error.c_str());
            return 1;
        }
        scoreMilliseconds += MillisecondsSince(start);
        ++scoredItems;
        size_t expectedBest = 0;
        size_t actualBest = 0;
        for (size_t i = 0; i < scores.size(); ++i) {
            const double reference = scoresValue->array[i].number;
            const double difference = std::abs(static_cast<double>(scores[i]) - reference);
            maxDifference = std::max(maxDifference, difference);
            ++scoreCount;
            if (difference <= kTolerance) {
                ++scoreWithin;
            } else if (shown++ < examples) {
                PrintUtf8("  \xE2\x9C\x97 採点 ");
                Print32(candidates[i]);
                std::printf("  macOS %.3f / Windows %.3f\n", reference, scores[i]);
            }
            if (reference > scoresValue->array[expectedBest].number) expectedBest = i;
            if (scores[i] > scores[actualBest]) actualBest = i;
        }
        if (expectedBest == actualBest) ++sameBest;
    }
    const int denominator = std::max(total, 1);
    std::printf("件数: %d  生成が一致: %d (%.1f%%)  平均 %.1fms/生成\n", total, sameGenerated,
                100.0 * sameGenerated / denominator, generateMilliseconds / denominator);
    std::printf("採点: %d 候補のうち差が %.2f 以内 %d (%.1f%%)  最大の差 %.4f  "
                "最良の候補が一致 %d/%d  平均 %.1fms/採点\n",
                scoreCount, kTolerance, scoreWithin, 100.0 * scoreWithin / std::max(scoreCount, 1),
                maxDifference, sameBest, scoredItems,
                scoreMilliseconds / std::max(scoredItems, 1));
    return sameGenerated == total && scoreWithin == scoreCount ? 0 : 2;
}

// モデルの採点（Score）が生成と辻褄が合っているかを、macOS 版の出力なしで確かめる。
// 入力は `model-parity` と同じ JSONL（reading・context・scoredCandidates を使う）。
//   1. 候補をまとめて採点した値と、1 つずつ採点した値が同じか（複数の系列を 1 バッチに載せる経路の確認）
//   2. 貪欲に生成した文をあらためて採点した値が、生成のときの対数確率と同じか
//      （採点は終端を「終端トークンのどれか」で測るので、採点の方がわずかに高くなることはある）
int RunModelSelfCheck(const std::wstring& referencePath, int limit) {
    std::ifstream input(std::filesystem::path(referencePath), std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "入力が読めません\n");
        return 1;
    }
    iroha::ZenzEngine zenz(DefaultModelPath(), ZenzOptions());
    std::string error;
    const auto fail = [&error] {
        std::fprintf(stderr, "エラー: %s\n", error.c_str());
        return 1;
    };
    int total = 0;
    double maxBatchDifference = 0.0;    // まとめて採点 と 1 つずつ採点 の差
    double maxGenerateDifference = 0.0; // 生成の対数確率 と 生成した文の採点 の差
    int generateBelow = 0;              // 採点の方が 0.01 より大きく低かった件数（あれば実装の誤り）
    int shownBatch = 0;
    std::string line;
    while (total < limit && std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        iroha::json::Value record;
        if (line.empty() || !iroha::json::Parse(line, &record)) continue;
        const iroha::json::Value* readingValue = record.Find("reading");
        const iroha::json::Value* candidatesValue = record.Find("scoredCandidates");
        if (!readingValue || !candidatesValue || candidatesValue->array.empty()) continue;
        const iroha::json::Value* contextValue = record.Find("context");
        const std::u32string reading = Utf8ToUtf32(readingValue->AsString());
        const std::u32string context = contextValue ? Utf8ToUtf32(contextValue->AsString()) : U"";
        std::vector<std::u32string> candidates;
        for (const iroha::json::Value& item : candidatesValue->array) {
            candidates.push_back(Utf8ToUtf32(item.AsString()));
        }
        ++total;

        std::vector<float> batched;
        if (!zenz.Score(candidates, reading, context, &batched, &error)) return fail();
        for (size_t i = 0; i < candidates.size(); ++i) {
            std::vector<float> single;
            if (!zenz.Score({candidates[i]}, reading, context, &single, &error)) return fail();
            const double batchDifference = std::abs(static_cast<double>(batched[i]) - single[0]);
            if (batchDifference > 0.01 && shownBatch++ < 8) {
                PrintUtf8("  \xE2\x9C\x97 "); // ✗
                Print32(candidates[i].substr(0, 30));
                std::printf("  [%zu/%zu] まとめて %.4f / 1つずつ %.4f\n", i, candidates.size(),
                            batched[i], single[0]);
            }
            maxBatchDifference = std::max(maxBatchDifference, batchDifference);
        }

        iroha::ZenzEngine::ScoredConversion generated;
        if (!zenz.ConvertScored(reading, context, &generated, &error)) return fail();
        std::vector<float> rescored;
        if (!zenz.Score({generated.text}, reading, context, &rescored, &error)) return fail();
        const double difference = static_cast<double>(rescored[0]) - generated.logProb;
        maxGenerateDifference = std::max(maxGenerateDifference, std::abs(difference));
        if (difference < -0.01) {
            ++generateBelow;
            if (generateBelow <= 5) {
                PrintUtf8("  \xE2\x9C\x97 "); // ✗
                Print32(generated.text);
                std::printf("  生成 %.4f / 採点 %.4f\n", generated.logProb, rescored[0]);
            }
        }
    }
    std::printf("件数: %d  まとめて採点と1つずつ採点の差の最大 %.5f  "
                "生成の対数確率と採点の差の最大 %.5f（採点の方が低い %d 件）\n",
                total, maxBatchDifference, maxGenerateDifference, generateBelow);
    return maxBatchDifference < 0.01 && generateBelow == 0 ? 0 : 2;
}

// ---- terms: 専門辞書 ----

// IME本体（設定画面）と同じ経路で、専門辞書の一覧を取り、入れ、外す。lookup は辞書名の印を引く
int RunTerms(const std::vector<std::u32string>& args) {
    const std::filesystem::path data = DataDir();
    const std::u32string sub = args.empty() ? U"list" : args[0];
    if (sub == U"lookup" && args.size() >= 3) {
        const auto lookup = iroha::TermDictionaryLookup::Load(
            data, iroha::TermDictionaryInstall::InstalledRecords(data));
        const std::u32string reading = RomajiToKana(args[1]);
        for (size_t i = 2; i < args.size(); ++i) {
            std::string labels;
            for (const std::u32string& label : lookup.Labels(reading, args[i])) {
                labels += (labels.empty() ? "" : "・") + Utf32ToUtf8(label);
            }
            PrintUtf8(Utf32ToUtf8(reading) + " → " + Utf32ToUtf8(args[i]) + "  " +
                      (labels.empty() ? std::string("（印なし）") : labels) + "\n");
        }
        return 0;
    }
    if (sub == U"remove" && args.size() >= 2) {
        const std::string id = Utf32ToUtf8(args[1]);
        if (!iroha::TermDictionaryInstall::Remove(data, id)) {
            std::fprintf(stderr, "外せませんでした: %s\n", id.c_str());
            return 1;
        }
        PrintUtf8("外しました: " + id + "\n");
        return 0;
    }

    const std::string url = iroha::TermDictionaryCatalog::DefaultUrl();
    iroha::TermDictionaryCatalog catalog;
    std::string error;
    if (!iroha::TermDictionaryFetcher::FetchCatalog(url, &catalog, &error)) {
        std::fprintf(stderr, "辞書一覧を取れません: %s\n", error.c_str());
        return 1;
    }
    const auto records = iroha::TermDictionaryInstall::InstalledRecords(data);
    if (sub == U"install" && args.size() >= 2) {
        const std::string id = Utf32ToUtf8(args[1]);
        const auto* dictionary = catalog.Find(id);
        if (!dictionary) {
            std::fprintf(stderr, "辞書一覧にありません: %s\n", id.c_str());
            return 1;
        }
        const auto started = std::chrono::steady_clock::now();
        iroha::TermDictionaryInstall::Record record;
        if (!iroha::TermDictionaryFetcher::Install(*dictionary, data, &record, &error)) {
            std::fprintf(stderr, "入れられませんでした: %s\n", error.c_str());
            return 1;
        }
        char line[256];
        std::snprintf(line, sizeof(line), "入れました: %s（%llu 語・%.1f 秒）\n", id.c_str(),
                      static_cast<unsigned long long>(record.entries),
                      MillisecondsSince(started) / 1000.0);
        PrintUtf8(line);
        return 0;
    }
    // list
    PrintUtf8(url + "\n");
    std::u32string group;
    for (const auto& dictionary : catalog.dictionaries) {
        if (dictionary.group != group) {
            group = dictionary.group;
            PrintUtf8("■ " + Utf32ToUtf8(group) + "\n");
        }
        std::string state = "未設置";
        for (const auto& record : records) {
            if (record.id != dictionary.id) continue;
            state = iroha::TermDictionaryInstall::NeedsUpdate(record, dictionary) ? "設置済み（更新あり）"
                                                                                  : "設置済み";
        }
        char line[512];
        std::snprintf(line, sizeof(line), "  %-24s %s  %llu 語  %s  [%s]\n", dictionary.id.c_str(),
                      Utf32ToUtf8(dictionary.name).c_str(),
                      static_cast<unsigned long long>(dictionary.entries),
                      Utf32ToUtf8(dictionary.license).c_str(), state.c_str());
        PrintUtf8(line);
    }
    return 0;
}

// ---- ai: AI変換 ----

// 変換サーバと同じく、config.json と資格情報マネージャーから接続先を作る
iroha::AIConnection AIConnectionFromConfig(const iroha::Config& config) {
    iroha::AIConnection connection;
    connection.service = iroha::ParseAIService(config.aiService);
    switch (connection.service) {
    case iroha::AIService::Ollama:
        connection.endpoint = config.ollamaEndpoint;
        connection.model = config.ollamaModel;
        break;
    case iroha::AIService::LMStudio:
        connection.endpoint = config.lmStudioEndpoint;
        connection.model = config.lmStudioModel;
        break;
    case iroha::AIService::OpenAI:
        connection.endpoint = config.openAIEndpoint;
        connection.model = config.openAIModel;
        iroha::SecretStore::Get(iroha::AIClient::kOpenAIKeyAccount, &connection.apiKey);
        break;
    case iroha::AIService::Claude:
        connection.model = config.claudeModel;
        iroha::SecretStore::Get(iroha::AIClient::kClaudeKeyAccount, &connection.apiKey);
        break;
    default:
        break;
    }
    return connection;
}

int RunAI(const std::vector<std::u32string>& args) {
    const iroha::Config config = iroha::LoadConfig(std::filesystem::path(DataDir()) / "config.json");
    const iroha::AIConnection connection = AIConnectionFromConfig(config);
    PrintUtf8("接続先: " + Utf32ToUtf8(iroha::AIServiceDisplayName(connection.service)) + "  モデル: " +
              (connection.model.empty() ? std::string("（既定）") : connection.model) + "\n");
    if (!args.empty() && args[0] == U"models") {
        std::vector<std::string> models;
        std::string error;
        if (!iroha::AIClient::ListModels(connection, &models, &error)) {
            std::fprintf(stderr, "モデルの一覧を取れません: %s\n", error.c_str());
            return 1;
        }
        for (const std::string& model : models) PrintUtf8("  " + model + "\n");
        return 0;
    }
    size_t preset = 0;
    size_t index = 0;
    if (args.size() >= 2 && args[0] == U"--preset") {
        preset = std::min<size_t>(std::stoul(Utf32ToUtf8(args[1])), iroha::AICommitSettings::kCount - 1);
        index = 2;
    }
    if (index >= args.size()) return 1;
    iroha::AICommitPreset settings;
    const auto& stored = config.aiPresets[preset];
    const auto& fallback = iroha::AICommitSettings::Defaults()[preset];
    settings.name = stored.name.value_or(fallback.name);
    settings.prompt = stored.prompt.value_or(fallback.prompt);
    const auto start = std::chrono::steady_clock::now();
    const iroha::AIResult result = iroha::AIClient::Run(
        connection, settings.Request(preset, args[index]),
        [&start](const std::u32string& partial) {
            char timing[32];
            std::snprintf(timing, sizeof(timing), "  [%.0fms] ", MillisecondsSince(start));
            PrintUtf8(timing + Utf32ToUtf8(partial) + "\n");
        },
        nullptr);
    if (!result.text) {
        std::fprintf(stderr, "失敗: %s\n", result.error.c_str());
        return 1;
    }
    char timing[32];
    std::snprintf(timing, sizeof(timing), "%.0fms", MillisecondsSince(start));
    PrintUtf8(Utf32ToUtf8(settings.DisplayName(preset)) + ": " + Utf32ToUtf8(*result.text) + "  [" + timing + "]\n");
    return 0;
}

// ---- typo: 打ち間違いの訂正 ----

std::filesystem::path TypoModelDirectory() {
    if (const wchar_t* env = _wgetenv(L"IROHA_TYPO_MODEL")) {
        if (env[0] != 0) return std::filesystem::path(env);
    }
    return iroha::TypoNormalizerInstall::Directory(DataDir());
}

// parity.json（書き出しに付いてくる、PyTorch 実装の出力）と突き合わせる。判定は macOS版の iroha-cli typo parity と同じ:
// float32 ならロジット 1e-3・logP 0.01 以内・生成が全件一致（float16 ならロジット 0.05・logP 0.1）
int RunTypoParity(const std::filesystem::path& directory) {
    std::ifstream file(directory / "parity.json", std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "parity.json がありません: %s\n", Utf32ToUtf8(Utf16ToUtf32(directory.wstring())).c_str());
        return 1;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    iroha::json::Value root;
    if (!iroha::json::Parse(buffer.str(), &root) || !root.Find("cases")) {
        std::fprintf(stderr, "parity.json を読めません\n");
        return 1;
    }
    std::ifstream manifestFile(directory / "manifest.json", std::ios::binary);
    std::stringstream manifestBuffer;
    manifestBuffer << manifestFile.rdbuf();
    iroha::json::Value manifest;
    iroha::json::Parse(manifestBuffer.str(), &manifest);
    const bool half = manifest.Find("dtype") && manifest.Find("dtype")->AsString() != "float32-le";
    const float logitTolerance = half ? 0.05f : 1e-3f;
    const double logTolerance = half ? 0.1 : 0.01;

    iroha::TypoNormalizer normalizer(directory);
    std::string error;
    if (!normalizer.Load(&error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    float worstLogit = 0;
    double worstLog = 0;
    int greedyMismatches = 0;
    int cases = 0;
    for (const auto& item : root.Find("cases")->array) {
        const std::u32string noisy = Utf8ToUtf32(item.Find("noisy")->AsString());
        const std::u32string clean = Utf8ToUtf32(item.Find("clean")->AsString());
        const std::u32string greedy = Utf8ToUtf32(item.Find("greedy")->AsString());
        ++cases;
        if (const auto* expected = item.Find("first_logits")) {
            const auto logits = normalizer.Logits(noisy, clean, expected->array.size());
            for (size_t step = 0; step < expected->array.size() && step < logits.size(); ++step) {
                const auto& row = expected->array[step].array;
                for (size_t j = 0; j < row.size() && j < logits[step].size(); ++j) {
                    worstLogit = std::max(worstLogit, std::fabs(static_cast<float>(row[j].number) - logits[step][j]));
                }
            }
        }
        const auto generated = normalizer.Generate(noisy);
        if (!generated || *generated != greedy) {
            ++greedyMismatches;
            if (greedyMismatches <= 10) {
                PrintUtf8("  生成が違う: " + Utf32ToUtf8(noisy) + " → " + Utf32ToUtf8(generated.value_or(U"(なし)")) +
                          "（期待 " + Utf32ToUtf8(greedy) + "）\n");
            }
        }
        const double logGreedy = normalizer.LogProbability(greedy, noisy);
        if (const auto* expected = item.Find("logprob_greedy")) {
            worstLog = std::max(worstLog, std::fabs(logGreedy - expected->number));
        }
        const auto* expectedNoisy = item.Find("logprob_noisy");
        if (expectedNoisy && expectedNoisy->type == iroha::json::Value::Type::Number) {
            worstLog = std::max(worstLog, std::fabs(normalizer.LogProbability(noisy, noisy) - expectedNoisy->number));
        }
    }
    char line[256];
    std::snprintf(line, sizeof(line),
                  "parity: %d 件  ロジットの最大誤差 %.6f（許容 %g）  logP の最大誤差 %.6f（許容 %g）  生成の不一致 %d 件  [%s]\n",
                  cases, worstLogit, logitTolerance, worstLog, logTolerance, greedyMismatches,
                  iroha::TypoNormalizer::KernelName());
    PrintUtf8(line);
    const bool ok = worstLogit <= logitTolerance && worstLog <= logTolerance && (half || greedyMismatches == 0);
    return ok ? 0 : 2;
}

int RunTypo(const std::vector<std::u32string>& args) {
    const std::u32string sub = args.empty() ? U"" : args[0];
    if (sub == U"catalog") {
        const std::string url = iroha::TypoNormalizerCatalog::DefaultUrl();
        iroha::TypoNormalizerCatalog catalog;
        std::string error;
        if (!iroha::TypoNormalizerFetcher::FetchCatalog(url, &catalog, &error)) {
            std::fprintf(stderr, "モデルの一覧を取れません: %s\n", error.c_str());
            return 1;
        }
        const std::filesystem::path data = DataDir();
        PrintUtf8(url + "\n");
        for (const auto& model : catalog.models) {
            char line[512];
            std::snprintf(line, sizeof(line), "  %-16s %s  %s  %.1f MB  %s%s\n", model.id.c_str(),
                          Utf32ToUtf8(model.name).c_str(), Utf32ToUtf8(model.summary).c_str(),
                          static_cast<double>(model.TotalBytes()) / 1e6, Utf32ToUtf8(catalog.LicenseFor(model)).c_str(),
                          iroha::TypoNormalizerInstall::NeedsUpdate(data, model) ? "" : "  [設置済み]");
            PrintUtf8(line);
        }
        if (args.size() >= 2 && args[1] == U"install") {
            // ID を指定しなければ、設定画面と同じく一覧の先頭のモデルを入れる
            const auto* found = args.size() >= 3 ? catalog.Find(Utf32ToUtf8(args[2])) : &catalog.models.front();
            if (!found) {
                std::fprintf(stderr, "モデルの一覧にありません: %s\n", Utf32ToUtf8(args[2]).c_str());
                return 1;
            }
            const auto& model = *found;
            const auto started = std::chrono::steady_clock::now();
            if (!iroha::TypoNormalizerFetcher::Install(model, catalog.LicenseFor(model), data, nullptr, &error)) {
                std::fprintf(stderr, "入れられませんでした: %s\n", error.c_str());
                return 1;
            }
            char line[256];
            std::snprintf(line, sizeof(line), "入れました: %s（%.1f 秒）\n", model.id.c_str(),
                          MillisecondsSince(started) / 1000.0);
            PrintUtf8(line);
        }
        return 0;
    }
    if (sub == U"parity") {
        std::filesystem::path directory = TypoModelDirectory();
        if (args.size() >= 3 && args[1] == U"--dir") directory = std::filesystem::path(Utf32ToUtf16(args[2]));
        return RunTypoParity(directory);
    }
    iroha::TypoNormalizer normalizer(TypoModelDirectory());
    std::string error;
    const auto loadStarted = std::chrono::steady_clock::now();
    if (!normalizer.Load(&error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const double loadMs = MillisecondsSince(loadStarted);
    if (sub == U"logits" && args.size() >= 3) {
        const size_t steps = args.size() >= 4 ? static_cast<size_t>(std::stoul(Utf32ToUtf8(args[3]))) : 3;
        const auto logits = normalizer.Logits(args[1], args[2], steps);
        std::string out = "[";
        for (size_t step = 0; step < logits.size(); ++step) {
            out += step == 0 ? "[" : ",[";
            for (size_t j = 0; j < logits[step].size(); ++j) {
                char value[32];
                std::snprintf(value, sizeof(value), "%s%.6f", j == 0 ? "" : ",", logits[step][j]);
                out += value;
            }
            out += "]";
        }
        PrintUtf8(out + "]\n");
        return 0;
    }
    double threshold = iroha::TypoNormalizer::kDefaultThreshold;
    size_t minimumLength = 1;
    size_t index = 0;
    while (index + 1 < args.size() && args[index].rfind(U"--", 0) == 0) {
        if (args[index] == U"--threshold") threshold = std::stod(Utf32ToUtf8(args[index + 1]));
        if (args[index] == U"--min-length") minimumLength = std::stoul(Utf32ToUtf8(args[index + 1]));
        index += 2;
    }
    char header[160];
    std::snprintf(header, sizeof(header), "読み込み %.0fms  行列の計算: %s  θ=%.2f\n", loadMs,
                  iroha::TypoNormalizer::KernelName(), threshold);
    PrintUtf8(header);
    for (; index < args.size(); ++index) {
        const std::u32string& reading = args[index];
        if (!normalizer.Supports(reading)) {
            PrintUtf8(Utf32ToUtf8(reading) + "  対象外（長すぎる・語彙に無い文字）\n");
            continue;
        }
        const auto generated = normalizer.Generate(reading);
        const double margin = generated && *generated != reading
                                  ? normalizer.LogProbability(*generated, reading) -
                                        normalizer.LogProbability(reading, reading)
                                  : 0.0;
        const auto started = std::chrono::steady_clock::now();
        const auto correction = normalizer.Correction(reading, threshold, minimumLength);
        const double ms = MillisecondsSince(started);
        char line[512];
        std::snprintf(line, sizeof(line), "%s  生成 %s  margin %.3f  → %s  [%.1fms]\n", Utf32ToUtf8(reading).c_str(),
                      Utf32ToUtf8(generated.value_or(U"(なし)")).c_str(), margin,
                      correction ? Utf32ToUtf8(correction->corrected).c_str() : "（訂正なし）", ms);
        PrintUtf8(line);
    }
    return 0;
}

// ---- symbols: 記号・顔文字・絵文字の表を引く ----

std::filesystem::path ExecutableDirectoryPath() {
    wchar_t modulePath[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(modulePath).parent_path();
}

std::string JoinUtf8(const std::vector<std::u32string>& values, const char* separator) {
    std::string joined;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) joined += separator;
        joined += Utf32ToUtf8(values[i]);
    }
    return joined;
}

int RunSymbols(const std::vector<std::u32string>& readings) {
    const auto directory = iroha::SymbolCandidateTable::DefaultDirectory(ExecutableDirectoryPath());
    if (!directory) {
        std::fprintf(stderr, "記号のデータが見つかりません（windows/scripts/fetch-symbols.ps1 で取得できます）\n");
        return 1;
    }
    // Windows で描けない絵文字を除いた表と、除かない表の両方を作って、除いた数を出す
    std::vector<std::u32string> excluded;
    auto predicate = iroha::EmojiRenderability::MakePredicate();
    const auto all = iroha::SymbolCandidateTable::Load(*directory);
    const auto table = iroha::SymbolCandidateTable::Load(
        *directory, [&](const std::u32string& emoji) {
            const bool renderable = !predicate || predicate(emoji);
            if (!renderable) excluded.push_back(emoji);
            return renderable;
        });
    if (!all || !table) {
        std::fprintf(stderr, "記号のデータを読めません\n");
        return 1;
    }
    const auto counts = table->GetCounts();
    char line[512];
    std::snprintf(line, sizeof(line), "%s  絵文字 %zu・顔文字 %zu・記号 %zu\n",
                  directory->u8string().c_str(), counts.emoji, counts.emoticons, counts.symbols);
    PrintUtf8(line);
    if (!predicate) {
        PrintUtf8("（DirectWrite を使えないため、描けない絵文字を除いていない）\n");
    } else {
        std::snprintf(line, sizeof(line), "Windows で描けないとして除いた絵文字 %zu 個（全 %zu 個）\n",
                      excluded.size(), all->GetCounts().emoji);
        PrintUtf8(line);
        const size_t shown = std::min<size_t>(excluded.size(), 40);
        PrintUtf8("  " + JoinUtf8(std::vector<std::u32string>(excluded.begin(), excluded.begin() + shown), " ") +
                  (shown < excluded.size() ? " …" : "") + "\n");
    }
    for (const std::u32string& argument : readings) {
        const std::u32string reading = RomajiToKana(argument);
        const iroha::SymbolCandidates found = table->Candidates(reading);
        PrintUtf8(Utf32ToUtf8(reading) + "\n");
        std::snprintf(line, sizeof(line), "  絵文字(%zu): ", found.emoji.size());
        PrintUtf8(line + JoinUtf8(found.emoji, " ") + "\n");
        std::snprintf(line, sizeof(line), "  顔文字(%zu): ", found.emoticons.size());
        PrintUtf8(line + JoinUtf8(found.emoticons, "  ") + "\n");
        std::snprintf(line, sizeof(line), "  記号(%zu): ", found.symbols.size());
        PrintUtf8(line + JoinUtf8(found.symbols, " ") + "\n");
    }
    return 0;
}

// ---- session: 変換サーバのセッションにキーを送る ----

struct SessionKeyStroke {
    iroha::SessionKey key = iroha::SessionKey::Character;
    char32_t character = 0;
    bool shift = false;
    std::u32string label;
    // <wait>: キーを送らず、ライブ変換・打ち間違いの訂正が届くまで待つ（--interval で続けて打つときの休止）
    bool wait = false;
    int pauseMs = 0;
};

// キー列を読む。<...> は特別なキー、それ以外は1文字ずつ
bool ParseKeyStrokes(const std::u32string& text, std::vector<SessionKeyStroke>* strokes) {
    static const std::pair<const char32_t*, iroha::SessionKey> kNames[] = {
        {U"space", iroha::SessionKey::Space}, {U"enter", iroha::SessionKey::Enter},
        {U"esc", iroha::SessionKey::Escape},  {U"bs", iroha::SessionKey::Backspace},
        {U"left", iroha::SessionKey::Left},   {U"right", iroha::SessionKey::Right},
        {U"up", iroha::SessionKey::Up},       {U"down", iroha::SessionKey::Down},
        {U"commit", iroha::SessionKey::CommitNow}, {U"reset", iroha::SessionKey::Reset},
        {U"f6", iroha::SessionKey::DisplayHiragana}, {U"f7", iroha::SessionKey::DisplayKatakana},
        {U"f8", iroha::SessionKey::DisplayHalfKatakana},
        {U"f9", iroha::SessionKey::DisplayFullAlphanumeric},
        {U"f10", iroha::SessionKey::DisplayHalfAlphanumeric},
        {U"punct", iroha::SessionKey::TogglePunctuation}, {U"undo", iroha::SessionKey::UndoCommit},
        {U"fix", iroha::SessionKey::AIFix},
        {U"vstart", iroha::SessionKey::VoiceStart}, {U"vstop", iroha::SessionKey::VoiceStop},
        {U"vcancel", iroha::SessionKey::VoiceCancel},
    };
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != U'<') {
            // 大文字は Shift を押して打ったものとして送る（TIP と同じ）
            const bool upper = text[i] >= U'A' && text[i] <= U'Z';
            strokes->push_back({iroha::SessionKey::Character, text[i], upper, std::u32string(1, text[i])});
            continue;
        }
        const size_t close = text.find(U'>', i);
        if (close == std::u32string::npos) return false;
        std::u32string name = text.substr(i + 1, close - i - 1);
        SessionKeyStroke stroke;
        stroke.label = U"<" + name + U">";
        if (name.rfind(U"S-", 0) == 0) {
            stroke.shift = true;
            name = name.substr(2);
        }
        bool found = false;
        if (name == U"wait") {
            stroke.wait = true;
            found = true;
        }
        // <pause1500>: キーを送らず、その時間だけ届いた表示を出す（「こえ」モードで話している間）
        if (name.size() > 5 && name.compare(0, 5, U"pause") == 0) {
            stroke.pauseMs = std::atoi(Utf32ToUtf8(name.substr(5)).c_str());
            found = stroke.pauseMs > 0;
        }
        if (name.size() == 3 && name.compare(0, 2, U"ai") == 0 && name[2] >= U'0' && name[2] <= U'2') {
            stroke.key = iroha::SessionKey::AICommit;
            stroke.character = name[2] - U'0';
            found = true;
        }
        for (const auto& entry : kNames) {
            if (name == entry.first) {
                stroke.key = entry.second;
                found = true;
            }
        }
        if (!found) return false;
        strokes->push_back(stroke);
        i = close;
    }
    return true;
}

// TIP が画面に出す内容を1行にする: 確定した文字列、未確定文字列（選んでいる文節は [ ] で囲む）、候補
void PrintSessionOutput(const std::u32string& label, const iroha::SessionOutput& output, double ms) {
    std::string line = Utf32ToUtf8(label);
    while (line.size() < 9) line += ' ';
    line += "| ";
    if (!output.commit.empty()) line += "確定「" + Utf32ToUtf8(output.commit) + "」 ";
    for (const auto& span : output.preedit) {
        const bool selected = span.style == iroha::SessionOutput::kSelectedSegment;
        line += (selected ? "[" : "") + Utf32ToUtf8(span.text) + (selected ? "]" : "");
        if (output.segmenting && !selected) line += " ";
    }
    if (output.livePending) line += " …";
    if (output.typoPending) line += " （訂正を確かめ中）";
    if (output.typoFeedback) {
        line += "  訂正: " + Utf32ToUtf8(output.typoFeedback->Original()) + " → " +
                Utf32ToUtf8(output.typoFeedback->Corrected()) + "（⌫ で戻す）";
    }
    if (output.aiFixPending) line += " （AIで直す: 結果待ち）";
    if (!output.aiFixNotice.empty()) line += "  〔" + Utf32ToUtf8(output.aiFixNotice) + "〕";
    if (!output.debugOverlay.empty()) {
        std::u32string overlay = output.debugOverlay;
        for (char32_t& c : overlay) {
            if (c == U'\n') c = U'｜';
        }
        line += "  〔デバッグ: " + Utf32ToUtf8(overlay) + "〕";
    }
    if (output.voicePending) line += " （こえ: 録音・認識中）";
    if (!output.voiceNotice.empty()) line += "  〔" + Utf32ToUtf8(output.voiceNotice) + "〕";
    if (output.aiFixFeedback) {
        line += "  AI: " + Utf32ToUtf8(output.aiFixFeedback->Original()) + " → " +
                Utf32ToUtf8(output.aiFixFeedback->Corrected()) + "（⌫ で戻す）";
    }
    if (output.candidatesVisible) {
        line += "  候補:";
        for (size_t i = 0; i < output.candidates.size(); ++i) {
            line += i == output.candidateIndex ? " *" : " ";
            line += Utf32ToUtf8(output.candidates[i]);
            // 専門辞書の名前の印
            if (i < output.candidateAnnotations.size() && !output.candidateAnnotations[i].empty()) {
                line += "[" + Utf32ToUtf8(output.candidateAnnotations[i]) + "]";
            }
        }
    }
    char timing[32];
    std::snprintf(timing, sizeof(timing), "  [%.1fms]", ms);
    PrintUtf8(line + timing + "\n");
}

int RunSession(const std::vector<SessionKeyStroke>& strokes, const std::u32string* context,
               int intervalMs) {
    // TIP と同じく、起動ごとに違う番号を使う
    const uint64_t sessionId =
        (static_cast<uint64_t>(GetCurrentProcessId()) << 32) | GetTickCount();
    const auto call = [](const std::vector<char>& request, iroha::ipc::SessionResponse* response) {
        std::vector<char> bytes;
        if (!iroha::ipc::Call(request, &bytes, 15000)) {
            std::fprintf(stderr, "サーバに接続できません（iroha-server.exe を起動してください）\n");
            return false;
        }
        *response = iroha::ipc::ParseSessionResponse(bytes);
        if (!response->ok) {
            std::fprintf(stderr, "サーバエラー: %s\n", response->error.c_str());
            return false;
        }
        return true;
    };
    // ライブ変換・打ち間違いの訂正が届くまで問い合わせる（TIP のタイマーにあたる）。届くたびに表示する
    const auto settle = [&](iroha::SessionOutput* output) {
        const auto start = std::chrono::steady_clock::now();
        while ((output->livePending || output->typoPending || output->aiPending || output->aiFixPending ||
                output->voicePending) &&
               MillisecondsSince(start) < 60000) {
            Sleep(5);
            iroha::ipc::SessionResponse response;
            if (!call(iroha::ipc::BuildSessionPollRequest(sessionId, output->revision), &response)) {
                return false;
            }
            if (!response.changed) continue;
            *output = response.output;
            PrintSessionOutput(U"  (届く)", *output, MillisecondsSince(start));
        }
        return true;
    };

    iroha::SessionOutput output;
    bool composing = false;
    for (const SessionKeyStroke& stroke : strokes) {
        if (stroke.wait) {
            if (!settle(&output)) return 1;
            continue;
        }
        if (stroke.pauseMs > 0) {
            // 決めた時間だけ問い合わせて、届いた表示を出す
            const auto start = std::chrono::steady_clock::now();
            while (MillisecondsSince(start) < stroke.pauseMs) {
                Sleep(20);
                iroha::ipc::SessionResponse response;
                if (!call(iroha::ipc::BuildSessionPollRequest(sessionId, output.revision), &response)) return 1;
                if (!response.changed) continue;
                output = response.output;
                PrintSessionOutput(U"  (届く)", output, MillisecondsSince(start));
            }
            continue;
        }
        const auto start = std::chrono::steady_clock::now();
        iroha::ipc::SessionResponse response;
        // 左文脈は、合成していないときのキーにだけ付ける（TIP と同じ）
        if (!call(iroha::ipc::BuildSessionKeyRequest(sessionId, stroke.key, stroke.character,
                                                     stroke.shift, composing ? nullptr : context),
                  &response)) {
            return 1;
        }
        output = response.output;
        composing = output.Composing();
        PrintSessionOutput(stroke.label, output, MillisecondsSince(start));
        // 録音を始めたキーのあとは待たない（離すまで終わらない）
        if (stroke.key == iroha::SessionKey::VoiceStart) continue;
        if (intervalMs < 0) {
            if (!settle(&output)) return 1;
        } else {
            Sleep(static_cast<DWORD>(intervalMs));
        }
    }
    if (!settle(&output)) return 1;
    std::vector<char> ignored;
    iroha::ipc::Call(iroha::ipc::BuildSessionEndRequest(sessionId), &ignored, 2000);
    return 0;
}

// 「こえ」モードの知らせを、届いた時刻つきで出す。終わったら語ごとの表記と読み・読みに戻した結果を出す
void PrintVoiceEvent(const iroha::InputSession::VoiceEvent& event, std::chrono::steady_clock::time_point start) {
    using Kind = iroha::InputSession::VoiceEvent::Kind;
    char timing[32];
    std::snprintf(timing, sizeof(timing), "[%6.0fms] ", MillisecondsSince(start));
    switch (event.kind) {
    case Kind::Started:
        PrintUtf8(std::string(timing) + "録音を始めた\n");
        break;
    case Kind::Partial:
        PrintUtf8(std::string(timing) + "途中: " + Utf32ToUtf8(event.text) + "\n");
        break;
    case Kind::Failed:
        PrintUtf8(std::string(timing) + "失敗: " + Utf32ToUtf8(event.text) + "\n");
        break;
    case Kind::Discarded:
        PrintUtf8(std::string(timing) + "捨てた\n");
        break;
    case Kind::Finished: {
        PrintUtf8(std::string(timing) + "結果: " + Utf32ToUtf8(event.text) + "\n");
        std::string words = "  語:";
        for (const iroha::ReadingToken& token : event.tokens) {
            words += " " + Utf32ToUtf8(token.surface) + (token.reading ? "/" + Utf32ToUtf8(*token.reading) : "/-");
        }
        PrintUtf8(words + "\n");
        const iroha::VoiceTranscript transcript(event.tokens);
        std::string parts = "  分けた結果:";
        for (const auto& part : transcript.parts) {
            parts += part.kind == iroha::VoiceTranscript::Part::Kind::Reading
                         ? " [" + Utf32ToUtf8(part.text) + "]"
                         : " {" + Utf32ToUtf8(part.text) + "}";
        }
        PrintUtf8(parts + "\n  読み: " + Utf32ToUtf8(transcript.ReadingText()) + "\n");
        break;
    }
    }
}

// 日本語の音声合成（SAPI）で文を読み上げて WAV（16kHz・モノラル・16 ビット）に書く（確認用の音声を作る）
bool SynthesizeSpeech(const std::wstring& path, const std::wstring& text) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    ISpVoice* voice = nullptr;
    ISpStream* stream = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&voice)))) {
        ISpObjectTokenCategory* category = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&category)))) {
            IEnumSpObjectTokens* tokens = nullptr;
            ISpObjectToken* token = nullptr;
            ULONG fetched = 0;
            if (SUCCEEDED(category->SetId(SPCAT_VOICES, FALSE)) &&
                SUCCEEDED(category->EnumTokens(L"Language=411", nullptr, &tokens)) && tokens &&
                SUCCEEDED(tokens->Next(1, &token, &fetched)) && token) {
                voice->SetVoice(token);
                token->Release();
            }
            if (tokens) tokens->Release();
            category->Release();
        }
        WAVEFORMATEX format = {WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0};
        if (SUCCEEDED(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&stream))) &&
            SUCCEEDED(stream->BindToFile(path.c_str(), SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx, &format, 0)) &&
            SUCCEEDED(voice->SetOutput(stream, TRUE))) {
            ok = SUCCEEDED(voice->Speak(text.c_str(), SPF_DEFAULT, nullptr));
            stream->Close();
        }
    }
    if (stream) stream->Release();
    if (voice) voice->Release();
    if (SUCCEEDED(initialized)) CoUninitialize();
    return ok;
}

int RunVoice(const std::vector<std::u32string>& args) {
    std::u32string engineName;
    if (!iroha::VoiceRecognizer::Available(&engineName)) {
        std::fprintf(stderr, "日本語の音声認識エンジンが見つかりません\n");
        return 1;
    }
    PrintUtf8("認識エンジン: " + Utf32ToUtf8(engineName) + "\n");
    bool stream = false;
    std::u32string say;
    size_t index = 0;
    if (args.size() >= 2 && args[0] == U"--mic") {
        const int seconds = std::max(1, std::atoi(Utf32ToUtf8(args[1]).c_str()));
        iroha::VoiceRecognizer recognizer;
        const auto start = std::chrono::steady_clock::now();
        recognizer.Start([start](const iroha::InputSession::VoiceEvent& event) { PrintVoiceEvent(event, start); });
        Sleep(static_cast<DWORD>(seconds * 1000));
        PrintUtf8("（録音をやめる）\n");
        recognizer.Stop();
        recognizer.Wait();
        return 0;
    }
    while (index < args.size() && args[index].rfind(U"--", 0) == 0) {
        if (args[index] == U"--stream") {
            stream = true;
            ++index;
        } else if (args[index] == U"--say" && index + 1 < args.size()) {
            say = args[index + 1];
            index += 2;
        } else {
            std::fprintf(stderr, "不明な引数: %s\n", Utf32ToUtf8(args[index]).c_str());
            return 2;
        }
    }
    if (index >= args.size()) {
        std::fprintf(stderr, "使い方: iroha-cli voice [--stream] [--say 文] <音声.wav>... | --mic <秒>\n");
        return 2;
    }
    int status = 0;
    for (; index < args.size(); ++index) {
        const std::wstring path = Utf32ToUtf16(args[index]);
        if (!say.empty()) {
            if (!SynthesizeSpeech(path, Utf32ToUtf16(say))) {
                std::fprintf(stderr, "音声合成に失敗しました\n");
                return 1;
            }
        }
        PrintUtf8("== " + Utf32ToUtf8(args[index]) + "\n");
        const auto start = std::chrono::steady_clock::now();
        std::string error;
        if (!iroha::VoiceRecognizer::RecognizeFile(
                path, stream, [start](const iroha::InputSession::VoiceEvent& event) { PrintVoiceEvent(event, start); },
                &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            status = 1;
        }
    }
    return status;
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

    if (command == U"terms") {
        return RunTerms(std::vector<std::u32string>(args.begin() + 2, args.end()));
    }

    if (command == U"ai") {
        return RunAI(std::vector<std::u32string>(args.begin() + 2, args.end()));
    }

    if (command == U"typo") {
        return RunTypo(std::vector<std::u32string>(args.begin() + 2, args.end()));
    }

    if (command == U"voice") {
        return RunVoice(std::vector<std::u32string>(args.begin() + 2, args.end()));
    }

    if (command == U"symbols") {
        return RunSymbols(std::vector<std::u32string>(args.begin() + 2, args.end()));
    }

    if (command == U"session" && args.size() >= 3) {
        std::u32string context;
        bool hasContext = false;
        int intervalMs = -1;
        size_t index = 2;
        while (index + 2 < args.size() && args[index].rfind(U"--", 0) == 0) {
            if (args[index] == U"--context") {
                context = args[index + 1];
                hasContext = true;
            } else if (args[index] == U"--interval") {
                intervalMs = std::atoi(Utf32ToUtf8(args[index + 1]).c_str());
            } else {
                std::fprintf(stderr, "不明な引数: %s\n", Utf32ToUtf8(args[index]).c_str());
                return 1;
            }
            index += 2;
        }
        std::vector<SessionKeyStroke> strokes;
        if (!ParseKeyStrokes(args[index], &strokes)) {
            std::fprintf(stderr, "キー列を読めません\n");
            return 1;
        }
        return RunSession(strokes, hasContext ? &context : nullptr, intervalMs);
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

    if (command == U"model-parity" && args.size() >= 3) {
        int examples = 10;
        for (size_t index = 3; index + 1 < args.size(); index += 2) {
            if (args[index] == U"--examples") {
                examples = std::atoi(Utf32ToUtf8(args[index + 1]).c_str());
            }
        }
        return RunModelParity(Utf32ToUtf16(args[2]), examples);
    }

    if (command == U"confidence" && args.size() >= 3) {
        std::u32string context;
        size_t index = 2;
        if (args[index] == U"--context" && index + 2 < args.size()) {
            context = args[index + 1];
            index += 2;
        }
        const std::u32string reading = RomajiToKana(args[index]);
        iroha::ZenzEngine zenz(DefaultModelPath(), ZenzOptions());
        iroha::ZenzEngine::ScoredConversion scored;
        std::string error;
        if (!zenz.ConvertScored(reading, context, &scored, &error)) {
            std::fprintf(stderr, "エラー: %s\n", error.c_str());
            return 1;
        }
        PrintUtf8(Utf32ToUtf8(reading) + " → " + Utf32ToUtf8(scored.text) + "\n");
        const auto characters = iroha::SplitGraphemes(scored.text);
        float minimum = std::numeric_limits<float>::infinity();
        for (size_t i = 0; i < characters.size() && i < scored.confidences.size(); ++i) {
            const auto& confidence = scored.confidences[i];
            minimum = std::min(minimum, confidence.margin);
            char line[96];
            std::snprintf(line, sizeof(line), "  %-4s logP %8.3f  margin %8.3f%s\n", Utf32ToUtf8(characters[i]).c_str(),
                          confidence.logProb, confidence.margin, confidence.relaxed ? "  （読み制約を外した）" : "");
            PrintUtf8(line);
        }
        char summary[64];
        std::snprintf(summary, sizeof(summary), "最小の margin %.3f（文字 %zu・自信度 %zu）\n", minimum, characters.size(),
                      scored.confidences.size());
        PrintUtf8(summary);
        return 0;
    }

    if (command == U"score" && args.size() >= 4) {
        // 候補ごとのモデルの対数確率を見る（調査用）: iroha-cli score [--context 文脈] <読み> <候補>...
        std::u32string context;
        size_t index = 2;
        if (args[index] == U"--context" && index + 2 < args.size()) {
            context = args[index + 1];
            index += 2;
        }
        const std::u32string reading = RomajiToKana(args[index++]);
        const std::vector<std::u32string> candidates(args.begin() + index, args.end());
        iroha::ZenzEngine zenz(DefaultModelPath(), ZenzOptions());
        std::vector<float> scores;
        std::string error;
        if (!zenz.Score(candidates, reading, context, &scores, &error)) {
            std::fprintf(stderr, "エラー: %s\n", error.c_str());
            return 1;
        }
        for (size_t i = 0; i < candidates.size(); ++i) {
            std::printf("%10.4f  ", scores[i]);
            Print32(candidates[i]);
            PrintUtf8("\n");
        }
        return 0;
    }

    if (command == U"model-selfcheck" && args.size() >= 3) {
        int limit = INT_MAX;
        for (size_t index = 3; index + 1 < args.size(); index += 2) {
            if (args[index] == U"--limit") limit = std::atoi(Utf32ToUtf8(args[index + 1]).c_str());
        }
        return RunModelSelfCheck(Utf32ToUtf16(args[2]), limit);
    }

    // bench / ajimee はモデルの素の力を測るため、既定でユーザ辞書・学習を空にする
    EngineStack stack(command == U"bench" || command == U"ajimee" ? UserDataMode::Evaluation
                                                                  : UserDataMode::Ime);
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
