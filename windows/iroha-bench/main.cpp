// 変換の精度と速度のベンチマークを測るヘルパー。設定画面（設定 > モデル > 変換のベンチマーク）が起動し、
// 標準出力の JSON Lines（iroha::benchmark_run::Event）で進み具合を受け取る。記録は <データフォルダ>\logs\bench\ に書く。
// 移植元: macos/Sources/iroha-bench/main.swift（引数・出力・記録の形は同じ）
//
// 使い方:
//   iroha-bench run --set <評価セット.jsonl|.json> --model <model.gguf>
//                   [--id <評価セットのID>] [--name <表示名>] [--adapter <adapter.gguf>]
//                   [--backend llama] [--lattice off|always] [--app-version <版>] [--limit N]
//   環境変数 IROHA_DATA_DIR でデータフォルダ（記録の置き場所）を上書き。無ければ %LOCALAPPDATA%\iroha
//
// ユーザ辞書・学習・専門辞書は使わない（iroha-cli ajimee と同じく、ほかの機械の数字と比べられるようにするため）。
// エンジンの重ね方は変換サーバと同じ（iroha::EngineAssembly）。
// 機械の状態の取り方は testdata/speed/README.md の「アプリ内のベンチマークの記録」の表（Windows の列）。
// 発熱の状態は Windows では取れないので書かない
#define NOMINMAX
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "iroha/benchmark.h"
#include "iroha/benchmark_run.h"
#include "iroha/engine_assembly.h"
#include "iroha/lattice_converter.h"
#include "iroha/unicode.h"
#include "iroha/zenz_engine.h"

namespace {

using iroha::benchmark_run::Event;

std::string Narrow(const std::wstring& text) { return iroha::Utf32ToUtf8(iroha::Utf16ToUtf32(text)); }
std::wstring Wide(const std::string& text) { return iroha::Utf32ToUtf16(iroha::Utf8ToUtf32(text)); }

void Emit(const Event& event) {
    const std::string line = event.JsonLine();
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

// 終える。静的な後始末（llama.cpp の GPU の解放など）は飛ばす（macOS版で Metal の解放が異常終了したため同じにする）
[[noreturn]] void Finish(int status) {
    std::fflush(stdout);
    std::fflush(stderr);
    _exit(status);
}

[[noreturn]] void Fail(const std::string& message) {
    Event event;
    event.kind = Event::Kind::Failed;
    event.message = message;
    Emit(event);
    Finish(1);
}

[[noreturn]] void Usage() {
    std::fputs("使い方:\n"
               "  iroha-bench run --set <評価セット.jsonl|.json> --model <model.gguf>\n"
               "                  [--id <ID>] [--name <表示名>] [--adapter <adapter.gguf>]\n"
               "                  [--backend llama] [--lattice off|always] [--app-version <版>] [--limit N]\n",
               stderr);
    std::fflush(stderr);
    _exit(2);
}

std::filesystem::path DataDirectory() {
    if (const wchar_t* env = _wgetenv(L"IROHA_DATA_DIR")) {
        if (env[0] != 0) return std::filesystem::path(env);
    }
    const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
    return std::filesystem::path(localAppData ? localAppData : L".") / L"iroha";
}

std::filesystem::path ExecutableDirectory() {
    wchar_t modulePath[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(modulePath).parent_path();
}

// ---- 機械の状態 ----

std::optional<std::string> RegistryString(const wchar_t* key, const wchar_t* name) {
    wchar_t buffer[512];
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::wstring text(buffer);
    const size_t first = text.find_first_not_of(L" \t");
    const size_t last = text.find_last_not_of(L" \t");
    if (first == std::wstring::npos) return std::nullopt;
    return Narrow(text.substr(first, last - first + 1));
}

// ホスト名（変換サーバのログと同じ GetComputerName）
std::string HostName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD length = ARRAYSIZE(name);
    if (!GetComputerNameW(name, &length)) return "windows";
    return Narrow(std::wstring(name, length));
}

// 機種（SMBIOS の製造元と製品名）
std::optional<std::string> MachineModel() {
    const wchar_t* key = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    const auto manufacturer = RegistryString(key, L"SystemManufacturer");
    const auto product = RegistryString(key, L"SystemProductName");
    if (manufacturer && product) return *manufacturer + " " + *product;
    if (product) return product;
    return manufacturer;
}

std::string CpuName() {
    return RegistryString(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString")
        .value_or("?");
}

// 「Windows 11 24H2 (26100)」（RtlGetVersion のビルド番号と DisplayVersion）
std::string OsVersion() {
    using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW version = {};
    version.dwOSVersionInfoSize = sizeof(version);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto get = reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(ntdll, "RtlGetVersion"))) {
            get(&version);
        }
    }
    const DWORD build = version.dwBuildNumber;
    std::string text = version.dwMajorVersion >= 10 ? (build >= 22000 ? "Windows 11" : "Windows 10")
                                                     : "Windows " + std::to_string(version.dwMajorVersion) + "." +
                                                           std::to_string(version.dwMinorVersion);
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    auto display = RegistryString(key, L"DisplayVersion");
    if (!display) display = RegistryString(key, L"ReleaseId");
    if (display) text += " " + *display;
    if (build > 0) text += " (" + std::to_string(build) + ")";
    return text;
}

// 電源（GetSystemPowerStatus の ACLineStatus）と省電力モード（SystemStatusFlag）。分からなければ無し
void PowerState(std::optional<std::string>* source, std::optional<bool>* saving) {
    SYSTEM_POWER_STATUS status = {};
    if (!GetSystemPowerStatus(&status)) return;
    if (status.ACLineStatus == 1) *source = "ac";
    if (status.ACLineStatus == 0) *source = "battery";
    if (status.SystemStatusFlag == 0 || status.SystemStatusFlag == 1) *saving = status.SystemStatusFlag == 1;
}

int64_t Now() { return static_cast<int64_t>(std::time(nullptr)); }

} // namespace

int wmain(int argc, wchar_t** argv) {
    // 標準出力は JSON Lines 専用（改行を CRLF にしない）
    _setmode(_fileno(stdout), _O_BINARY);

    if (argc < 2 || std::wstring(argv[1]) != L"run") Usage();
    std::map<std::string, std::string> options;
    for (int index = 2; index < argc; index += 2) {
        const std::wstring key = argv[index];
        if (key.rfind(L"--", 0) != 0 || index + 1 >= argc) Usage();
        options[Narrow(key.substr(2))] = Narrow(argv[index + 1]);
    }
    if (!options.count("set") || !options.count("model")) Usage();
    const auto lattice = iroha::benchmark_run::ParseLattice(options.count("lattice") ? options["lattice"] : "off");
    if (!lattice) Usage();
    const std::string backend = options.count("backend") ? options["backend"] : "llama";
    if (backend != "llama" && backend != "mlx") Usage();
    if (backend == "mlx") Fail("Windows版の推論エンジンは llama.cpp だけです");
    const std::string adapterPath = options.count("adapter") ? options["adapter"] : "";
    const std::filesystem::path setPath(Wide(options["set"]));
    const std::filesystem::path modelPath(Wide(options["model"]));
    const std::filesystem::path dataDirectory = DataDirectory();

    std::vector<iroha::benchmark::Item> items;
    std::string error;
    if (!iroha::benchmark::LoadItems(setPath, &items, &error)) Fail("評価セットが読めません: " + error);
    if (options.count("limit")) {
        const int limit = std::atoi(options["limit"].c_str());
        if (limit > 0 && static_cast<size_t>(limit) < items.size()) items.resize(static_cast<size_t>(limit));
    }
    if (!std::filesystem::exists(modelPath)) Fail("モデルがありません: " + options["model"]);
    const std::string benchmarkID = options.count("id") ? options["id"] : setPath.stem().u8string();
    const std::string benchmarkName = options.count("name") ? options["name"] : benchmarkID;

    // ---- エンジン ----

    iroha::ZenzEngine::Options zenzOptions;
    zenzOptions.adapterPath = adapterPath;
    iroha::ZenzEngine zenz(options["model"], zenzOptions);
    std::unique_ptr<iroha::LatticeConverter> latticeConverter;
    if (const auto dictionary = iroha::LatticeConverter::DefaultDictionaryPath(ExecutableDirectory())) {
        latticeConverter = std::make_unique<iroha::LatticeConverter>(*dictionary);
    }
    if (*lattice == iroha::benchmark_run::Lattice::Always && !latticeConverter) Fail("辞書ラティスの辞書が見つかりません");
    iroha::EngineAssembly assembly(&zenz, &zenz, latticeConverter.get(),
                                   *lattice == iroha::benchmark_run::Lattice::Always ? iroha::LatticeUse::FirstCandidate
                                                                                    : iroha::LatticeUse::Off);
    iroha::ConversionEngine& engine = assembly.Engine();

    // ---- 計測 ----

    const std::string hostName = HostName();
    const int64_t startedAt = Now();
    const DWORD startedTick = GetTickCount();

    Event loading;
    loading.kind = Event::Kind::Loading;
    loading.done = 0;
    loading.total = static_cast<int>(items.size());
    Emit(loading);
    const double warmUp = iroha::benchmark::WarmUp(engine);
    // 読み込めなかった（モデルが壊れている・アダプタが合わない）なら、ここで止める
    std::string loadError;
    if (!zenz.Prewarm(&loadError)) Fail("モデルを読み込めません: " + loadError);
    // 推論の実行先とスレッド数（llama.cpp は読み込んだあとでないと分からない）
    iroha::ZenzEngine::RuntimeInfo runtime;
    const bool hasRuntime = zenz.GetRuntimeInfo(&runtime);

    std::vector<iroha::benchmark::ItemResult> results;
    results.reserve(items.size());
    int hits = 0;
    for (const iroha::benchmark::Item& item : items) {
        results.push_back(iroha::benchmark::Run(item, engine));
        if (results.back().hit) ++hits;
        Event progress;
        progress.kind = Event::Kind::Progress;
        progress.done = static_cast<int>(results.size());
        progress.total = static_cast<int>(items.size());
        progress.hits = hits;
        Emit(progress);
    }

    // 計測中に入力があれば、その変換と CPU・GPU を取り合っている（GetLastInputInfo はマウスの動きも含む）
    LASTINPUTINFO lastInput = {sizeof(lastInput)};
    std::optional<bool> inputDuringRun;
    if (GetLastInputInfo(&lastInput)) inputDuringRun = static_cast<LONG>(lastInput.dwTime - startedTick) > 0;

    iroha::benchmark_run::Environment environment;
    environment.platform = "windows";
#ifdef IROHA_VERSION
    environment.appVersion = options.count("app-version") && !options["app-version"].empty() ? options["app-version"]
                                                                                             : IROHA_VERSION;
#else
    environment.appVersion = options.count("app-version") ? options["app-version"] : "?";
#endif
    environment.hostName = hostName;
    environment.machineModel = MachineModel();
    environment.cpu = CpuName();
    environment.cpuCores = static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    environment.osVersion = OsVersion();
    PowerState(&environment.powerSource, &environment.powerSaving);

    iroha::benchmark_run::Record record;
    record.id = iroha::benchmark_run::MakeId(startedAt, hostName);
    record.benchmarkID = benchmarkID;
    record.benchmarkName = benchmarkName;
    record.condition.lattice = *lattice;
    record.condition.backend = backend;
    record.condition.model = modelPath.filename().u8string();
    if (!adapterPath.empty()) record.condition.adapter = std::filesystem::path(Wide(adapterPath)).filename().u8string();
    if (hasRuntime) {
        record.condition.device = runtime.device;
        record.condition.threads = runtime.threads;
    }
    record.environment = environment;
    record.startedAt = startedAt;
    record.finishedAt = Now();
    record.warmUpMilliseconds = warmUp;
    record.inputDuringRun = inputDuringRun;
    record.summary = iroha::benchmark::Summary::Of(results);
    record.misses = iroha::benchmark_run::Record::Misses(items, results);
    if (!iroha::benchmark_run::Write(dataDirectory, record, results, &error)) Fail("記録を書けませんでした: " + error);

    Event finished;
    finished.kind = Event::Kind::Finished;
    finished.done = static_cast<int>(results.size());
    finished.total = static_cast<int>(items.size());
    finished.hits = hits;
    finished.recordID = record.id;
    Emit(finished);
    Finish(0);
}
