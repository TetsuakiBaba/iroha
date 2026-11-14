// iroha の IBus エンジンのプロセス。
//   ibus-engine-iroha --ibus   ibus-daemon がコンポーネントの定義（iroha.xml）から起動する
//   ibus-engine-iroha          開発用。起動したまま IBus に「iroha-dev」として登録する（インストールしたものと並べて
//                              試せる。`ibus engine iroha-dev` で切り替える。確定は学習に残るので、試すときは
//                              IROHA_DATA_DIR で別のデータフォルダを指し、IROHA_MODEL でモデルを渡す）
//
// 変換エンジン一式（EngineHost）とセッションの表（SessionManager）はプロセスで 1 つ。モデル・辞書は
// IBus につなぐ前に読み込む（最初のキーでもたつかないように）。
// 設定画面が <データフォルダ>/reload-request を書き換えたら読み直す（Windows版の IPC の Reload にあたる）。
// 起動・終了は <データフォルダ>/logs/launch-<ホスト名>.log に残す（LaunchLog）
#include <glib-unix.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include <ibus.h>

#include "emoji_renderability.h"
#include "engine.h"
#include "engine_host.h"
#include "launch_log.h"
#include "iroha/platform.h"
#include "iroha/session_manager.h"
#include "iroha/unicode.h"

#ifndef IROHA_VERSION
#define IROHA_VERSION "dev"
#endif

namespace {

constexpr char kComponentName[] = "org.freedesktop.IBus.Iroha";
constexpr char kEngineName[] = "iroha";
constexpr char kDevelopmentComponentName[] = "org.freedesktop.IBus.IrohaDev";
constexpr char kDevelopmentEngineName[] = "iroha-dev";
// 記録のファイルがこれを超えていたら、起動のときに 1 世代だけ残して作り直す
constexpr uintmax_t kMaxLogBytes = 1 << 20;

// 動作の記録: <データフォルダ>/logs/ibus-iroha-<ホスト名>.log と標準エラー
class FileLog {
public:
    FileLog() {
        const std::filesystem::path directory = iroha::platform::DataDirectory() / "logs";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        path_ = directory / iroha::PathFromUtf8("ibus-iroha-" + iroha::platform::HostName() + ".log");
        if (std::filesystem::file_size(path_, error) > kMaxLogBytes && !error) {
            std::filesystem::path old = path_;
            old += ".1";
            std::filesystem::rename(path_, old, error);
        }
    }

    void Write(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
        const std::string line = std::string(stamp) + " [" + std::to_string(getpid()) + "] " + message + "\n";
        std::ofstream file(path_, std::ios::binary | std::ios::app);
        file << line;
        std::fputs(line.c_str(), stderr);
    }

private:
    std::filesystem::path path_;
    std::mutex mutex_;
};

// 終わった理由（起動の記録に書く）
std::string g_exitReason = "disconnected";

void OnDisconnected(IBusBus*, gpointer) { ibus_quit(); }

gboolean OnSignal(gpointer data) {
    g_exitReason = static_cast<const char*>(data);
    ibus_quit();
    return G_SOURCE_REMOVE;
}

// 設定の読み直し（設定画面が reload-request を書き換えたら。続けて書かれても 1 回にまとめる）
struct Reloader {
    iroha::EngineHost* host = nullptr;
    iroha::SessionManager* sessions = nullptr;
    std::function<void(const std::string&)> log;
    guint timer = 0;
};

gboolean Reload(gpointer data) {
    Reloader& reloader = *static_cast<Reloader*>(data);
    reloader.timer = 0;
    reloader.log("reload requested");
    reloader.sessions->WithEngine([&reloader] { reloader.host->Build(); });
    reloader.host->ApplyTo(*reloader.sessions);
    iroha::ibus::SettingsReloaded();
    return G_SOURCE_REMOVE;
}

void OnReloadRequest(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent event, gpointer data) {
    if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT && event != G_FILE_MONITOR_EVENT_CREATED &&
        event != G_FILE_MONITOR_EVENT_CHANGED) {
        return;
    }
    Reloader& reloader = *static_cast<Reloader*>(data);
    if (reloader.timer == 0) reloader.timer = g_timeout_add(300, Reload, &reloader);
}

// 開発用のコンポーネント（インストールしたものと名前を分ける）
IBusComponent* MakeDevelopmentComponent(const std::string& executable) {
    IBusComponent* component = ibus_component_new_varargs(
        "name", kDevelopmentComponentName, "description", "iroha（開発用）", "version", IROHA_VERSION,
        "license", "MIT", "author", "Tetsuaki Baba", "homepage", "https://github.com/TetsuakiBaba/iroha",
        "command-line", executable.c_str(), "textdomain", "iroha", nullptr);
    IBusEngineDesc* description = ibus_engine_desc_new_varargs(
        "name", kDevelopmentEngineName, "longname", "iroha（開発用）", "description", "iroha（開発用）", "language", "ja",
        "license", "MIT", "author", "Tetsuaki Baba", "layout", "default", "symbol", "あ", "rank", 0, nullptr);
    ibus_component_add_engine(component, description);
    return component;
}

} // namespace

int main(int argc, char** argv) {
    bool launchedByIbus = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ibus") == 0 || std::strcmp(argv[i], "-i") == 0) {
            launchedByIbus = true;
        } else if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("ibus-engine-iroha %s\n", IROHA_VERSION);
            return 0;
        } else {
            std::fprintf(stderr, "使い方: %s [--ibus] [--version]\n", argv[0]);
            return 2;
        }
    }

    FileLog fileLog;
    const auto log = [&fileLog](const std::string& message) { fileLog.Write(message); };
    log(std::string("start ") + IROHA_VERSION + (launchedByIbus ? " (ibus)" : " (standalone)"));
    // 起動の記録（インストールしたものだけ。開発用は残さない）
    iroha::LaunchLog launchLog(iroha::platform::DataDirectory() / "logs", iroha::platform::HostName(), "ibus-engine-iroha");
    if (launchedByIbus) {
        for (const std::string& line : launchLog.RecordLaunch(IROHA_VERSION)) log("launch log: " + line);
    }

    const auto started = std::chrono::steady_clock::now();
    iroha::EngineHost::Options options;
    options.log = log;
    // 描けない絵文字（フォントが古い・国旗など）は記号の表に入れない
    options.isRenderableEmoji = iroha::EmojiRenderability::MakePredicate();
    if (!options.isRenderableEmoji) log("emoji font not found (all emoji are kept)");
    iroha::EngineHost host(options);
    host.Build();
    iroha::SessionManager sessions(&host);
    host.ApplyTo(sessions);
    iroha::ibus::SetShared(&host, &sessions, log);
    log("engine ready (" +
        std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()) +
        "ms)");

    ibus_init();
    IBusBus* bus = ibus_bus_new();
    if (!ibus_bus_is_connected(bus)) {
        log("cannot connect to ibus-daemon");
        return 1;
    }
    g_signal_connect(bus, "disconnected", G_CALLBACK(OnDisconnected), nullptr);
    IBusFactory* factory = ibus_factory_new(ibus_bus_get_connection(bus));
    ibus_factory_add_engine(factory, launchedByIbus ? kEngineName : kDevelopmentEngineName, IROHA_TYPE_ENGINE);
    if (launchedByIbus) {
        if (ibus_bus_request_name(bus, kComponentName, 0) == 0) {
            log("cannot own the bus name");
            return 1;
        }
    } else {
        const std::string executable = iroha::PathToUtf8(iroha::platform::ExecutableDirectory() / "ibus-engine-iroha");
        if (!ibus_bus_register_component(bus, MakeDevelopmentComponent(executable))) {
            log("cannot register the component");
            return 1;
        }
        log("registered as iroha-dev (data: " + iroha::PathToUtf8(iroha::platform::DataDirectory()) + ")");
    }
    // 設定の読み直しの依頼を見張る
    Reloader reloader{&host, &sessions, log, 0};
    GFile* reloadFile = g_file_new_for_path(iroha::PathToUtf8(iroha::platform::ReloadRequestPath()).c_str());
    GFileMonitor* reloadMonitor = g_file_monitor_file(reloadFile, G_FILE_MONITOR_NONE, nullptr, nullptr);
    if (reloadMonitor) g_signal_connect(reloadMonitor, "changed", G_CALLBACK(OnReloadRequest), &reloader);
    g_unix_signal_add(SIGTERM, OnSignal, const_cast<char*>("SIGTERM"));
    g_unix_signal_add(SIGINT, OnSignal, const_cast<char*>("SIGINT"));
    g_unix_signal_add(SIGHUP, OnSignal, const_cast<char*>("SIGHUP"));

    ibus_main();
    log("stop (" + g_exitReason + ")");
    if (reloader.timer) g_source_remove(reloader.timer);
    if (reloadMonitor) g_object_unref(reloadMonitor);
    g_object_unref(reloadFile);
    iroha::platform::SetComposingFlag(false);
    if (launchedByIbus) launchLog.RecordExit(g_exitReason);
    iroha::ibus::SetShared(nullptr, nullptr, nullptr);
    return 0;
}
