// 変換の精度と速度のベンチマークを測るヘルパー。設定画面（設定 > モデル > 変換のベンチマーク）が起動し、
// 標準出力の JSON Lines（iroha::benchmark_run::Event）で進み具合を受け取る。記録は <データフォルダ>/logs/bench/ に書く。
// 移植元: macos/Sources/iroha-bench/main.swift（引数・出力・記録の形は同じ。Windows版 windows/iroha-bench も同じ）
//
// 使い方:
//   iroha-bench run --set <評価セット.jsonl|.json> --model <model.gguf>
//                   [--id <評価セットのID>] [--name <表示名>] [--adapter <adapter.gguf>]
//                   [--backend llama] [--lattice off|always] [--app-version <版>] [--limit N]
//   環境変数 IROHA_DATA_DIR でデータフォルダ（記録の置き場所）を上書き。無ければ $XDG_DATA_HOME/iroha か ~/.local/share/iroha
//
// ユーザ辞書・学習・専門辞書は使わない（iroha-cli ajimee と同じく、ほかの機械の数字と比べられるようにするため）。
// エンジンの重ね方は IBus・fcitx5 のエンジンと同じ（iroha::EngineAssembly）。
// 機械の状態の取り方は testdata/speed/README.md の「アプリ内のベンチマークの記録」の表（Linux の列）。
// 発熱の状態は書かない。計測中の入力は X11 なら XScreenSaver の idle（libXss を実行時に読み込む）で見て、
// Wayland・画面の無い環境では書かない
#include <dlfcn.h>
#include <gio/gio.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "iroha/benchmark.h"
#include "iroha/benchmark_run.h"
#include "iroha/engine_assembly.h"
#include "iroha/lattice_converter.h"
#include "iroha/platform.h"
#include "iroha/unicode.h"
#include "iroha/zenz_engine.h"

// X11 のヘッダは iroha のヘッダの後に読み、名前のぶつかるマクロ（Always。benchmark_run::Lattice::Always）を外す
#include <X11/Xlib.h>
#undef Always

namespace {

using iroha::benchmark_run::Event;

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

// ---- 機械の状態 ----

std::optional<std::string> ReadTrimmed(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) return std::nullopt;
    std::string text;
    std::getline(file, text);
    const size_t first = text.find_first_not_of(" \t\r\n");
    const size_t last = text.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) return std::nullopt;
    return text.substr(first, last - first + 1);
}

// 機種（/sys/class/dmi/id の製造元と製品名）
std::optional<std::string> MachineModel() {
    const auto vendor = ReadTrimmed("/sys/class/dmi/id/sys_vendor");
    const auto product = ReadTrimmed("/sys/class/dmi/id/product_name");
    if (vendor && product) return *vendor + " " + *product;
    if (product) return product;
    return vendor;
}

// /proc/cpuinfo の最初の model name
std::string CpuName() {
    std::ifstream file("/proc/cpuinfo");
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("model name", 0) != 0) continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const size_t first = line.find_first_not_of(" \t", colon + 1);
        if (first != std::string::npos) return line.substr(first);
    }
    return "?";
}

// /etc/os-release の PRETTY_NAME（「Ubuntu 22.04.5 LTS」）
std::string OsVersion() {
    std::ifstream file("/etc/os-release");
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
        std::string value = line.substr(std::strlen("PRETTY_NAME="));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }
        if (!value.empty()) return value;
    }
    return "Linux";
}

// 電源（/sys/class/power_supply の種類 Mains の online）。Mains が無い機械（据え置きの多く）では無し
std::optional<std::string> PowerSource() {
    std::error_code error;
    bool hasMains = false;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/power_supply", error)) {
        if (ReadTrimmed(entry.path() / "type").value_or("") != "Mains") continue;
        hasMains = true;
        if (ReadTrimmed(entry.path() / "online").value_or("") == "1") return std::string("ac");
    }
    if (hasMains) return std::string("battery");
    return std::nullopt;
}

// 省電力（power-profiles-daemon の ActiveProfile が power-saver か）。デーモンが無ければ無し
std::optional<bool> PowerSaving() {
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    if (!bus) return std::nullopt;
    std::optional<bool> saving;
    // 新しい版は org.freedesktop.UPower.PowerProfiles、古い版は net.hadess.PowerProfiles
    const char* names[][2] = {{"org.freedesktop.UPower.PowerProfiles", "/org/freedesktop/UPower/PowerProfiles"},
                              {"net.hadess.PowerProfiles", "/net/hadess/PowerProfiles"}};
    for (const auto& name : names) {
        GVariant* reply = g_dbus_connection_call_sync(
            bus, name[0], name[1], "org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", name[0], "ActiveProfile"), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000,
            nullptr, nullptr);
        if (!reply) continue;
        GVariant* value = nullptr;
        g_variant_get(reply, "(v)", &value);
        if (value && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
            saving = std::strcmp(g_variant_get_string(value, nullptr), "power-saver") == 0;
        }
        if (value) g_variant_unref(value);
        g_variant_unref(reply);
        if (saving) break;
    }
    g_object_unref(bus);
    return saving;
}

// 入力が無かった時間（ミリ秒。X11 の XScreenSaver の idle。マウスの動きも含む）。
// Wayland（X11 のクライアントの入力しか見えない）・画面の無い環境・libXss が無ければ無し。
// libXss の開発用ファイルを要らなくするため、実行時に読み込む（構造体は scrnsaver.h と同じ並び）
std::optional<unsigned long> InputIdleMilliseconds() {
    const char* session = std::getenv("XDG_SESSION_TYPE");
    if ((session && std::strcmp(session, "wayland") == 0) || std::getenv("WAYLAND_DISPLAY")) return std::nullopt;
    if (!std::getenv("DISPLAY")) return std::nullopt;
    struct ScreenSaverInfo {
        Window window;
        int state;
        int kind;
        unsigned long tilOrSince;
        unsigned long idle;
        unsigned long eventMask;
    };
    using QueryExtension = Bool (*)(Display*, int*, int*);
    using AllocInfo = ScreenSaverInfo* (*)();
    using QueryInfo = Status (*)(Display*, Drawable, ScreenSaverInfo*);
    void* library = dlopen("libXss.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!library) return std::nullopt;
    auto queryExtension = reinterpret_cast<QueryExtension>(dlsym(library, "XScreenSaverQueryExtension"));
    auto allocInfo = reinterpret_cast<AllocInfo>(dlsym(library, "XScreenSaverAllocInfo"));
    auto queryInfo = reinterpret_cast<QueryInfo>(dlsym(library, "XScreenSaverQueryInfo"));
    std::optional<unsigned long> idle;
    if (queryExtension && allocInfo && queryInfo) {
        if (Display* display = XOpenDisplay(nullptr)) {
            int eventBase = 0;
            int errorBase = 0;
            if (queryExtension(display, &eventBase, &errorBase)) {
                if (ScreenSaverInfo* info = allocInfo()) {
                    if (queryInfo(display, DefaultRootWindow(display), info)) idle = info->idle;
                    XFree(info);
                }
            }
            XCloseDisplay(display);
        }
    }
    dlclose(library);
    return idle;
}

int64_t Now() { return static_cast<int64_t>(std::time(nullptr)); }

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) != "run") Usage();
    std::map<std::string, std::string> options;
    for (int index = 2; index < argc; index += 2) {
        const std::string key = argv[index];
        if (key.rfind("--", 0) != 0 || index + 1 >= argc) Usage();
        options[key.substr(2)] = argv[index + 1];
    }
    if (!options.count("set") || !options.count("model")) Usage();
    const auto lattice = iroha::benchmark_run::ParseLattice(options.count("lattice") ? options["lattice"] : "off");
    if (!lattice) Usage();
    const std::string backend = options.count("backend") ? options["backend"] : "llama";
    if (backend != "llama" && backend != "mlx") Usage();
    if (backend == "mlx") Fail("Linux版の推論エンジンは llama.cpp だけです");
    const std::string adapterPath = options.count("adapter") ? options["adapter"] : "";
    const std::filesystem::path setPath = iroha::PathFromUtf8(options["set"]);
    const std::filesystem::path modelPath = iroha::PathFromUtf8(options["model"]);
    const std::filesystem::path dataDirectory = iroha::platform::DataDirectory();

    std::vector<iroha::benchmark::Item> items;
    std::string error;
    if (!iroha::benchmark::LoadItems(setPath, &items, &error)) Fail("評価セットが読めません: " + error);
    if (options.count("limit")) {
        const int limit = std::atoi(options["limit"].c_str());
        if (limit > 0 && static_cast<size_t>(limit) < items.size()) items.resize(static_cast<size_t>(limit));
    }
    if (!std::filesystem::exists(modelPath)) Fail("モデルがありません: " + options["model"]);
    const std::string benchmarkID = options.count("id") ? options["id"] : iroha::PathToUtf8(setPath.stem());
    const std::string benchmarkName = options.count("name") ? options["name"] : benchmarkID;

    // ---- エンジン ----

    iroha::ZenzEngine::Options zenzOptions;
    zenzOptions.adapterPath = adapterPath;
    iroha::ZenzEngine zenz(options["model"], zenzOptions);
    std::unique_ptr<iroha::LatticeConverter> latticeConverter;
    if (const auto dictionary = iroha::LatticeConverter::DefaultDictionaryPath(iroha::platform::ExecutableDirectory())) {
        latticeConverter = std::make_unique<iroha::LatticeConverter>(*dictionary);
    }
    if (*lattice == iroha::benchmark_run::Lattice::Always && !latticeConverter) Fail("辞書ラティスの辞書が見つかりません");
    iroha::EngineAssembly assembly(&zenz, &zenz, latticeConverter.get(),
                                   *lattice == iroha::benchmark_run::Lattice::Always ? iroha::LatticeUse::FirstCandidate
                                                                                    : iroha::LatticeUse::Off);
    iroha::ConversionEngine& engine = assembly.Engine();

    // ---- 計測 ----

    const std::string hostName = iroha::platform::HostName();
    const int64_t startedAt = Now();
    const auto startedClock = std::chrono::steady_clock::now();

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

    // 計測中に入力があれば、その変換と CPU を取り合っている（入力が無かった時間が計測の時間より短ければ、あった）
    std::optional<bool> inputDuringRun;
    if (const auto idle = InputIdleMilliseconds()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - startedClock)
                                 .count();
        inputDuringRun = static_cast<long long>(*idle) < static_cast<long long>(elapsed);
    }

    iroha::benchmark_run::Environment environment;
    environment.platform = "linux";
#ifdef IROHA_VERSION
    environment.appVersion = options.count("app-version") && !options["app-version"].empty() ? options["app-version"]
                                                                                             : IROHA_VERSION;
#else
    environment.appVersion = options.count("app-version") ? options["app-version"] : "?";
#endif
    environment.hostName = hostName;
    environment.machineModel = MachineModel();
    environment.cpu = CpuName();
    const long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > 0) environment.cpuCores = static_cast<int>(cores);
    environment.osVersion = OsVersion();
    environment.powerSource = PowerSource();
    environment.powerSaving = PowerSaving();

    iroha::benchmark_run::Record record;
    record.id = iroha::benchmark_run::MakeId(startedAt, hostName);
    record.benchmarkID = benchmarkID;
    record.benchmarkName = benchmarkName;
    record.condition.lattice = *lattice;
    record.condition.backend = backend;
    record.condition.model = iroha::PathToUtf8(modelPath.filename());
    if (!adapterPath.empty()) record.condition.adapter = iroha::PathToUtf8(iroha::PathFromUtf8(adapterPath).filename());
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
