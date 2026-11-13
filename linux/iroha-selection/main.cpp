// iroha-selection: 選択テキストのAI編集の常駐プログラム（X11）。
//   ほかのアプリで選んだ文章を、グローバルショートカットで AI に渡し、結果の窓から置き換える（既定 OFF）。
//   ログインのときに /etc/xdg/autostart/jp.iroha.Selection.desktop から起動する。設定（config.json の
//   selectionEnabled）が OFF の間は何もつかまない。Wayland のセッションでは何もしない
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "iroha/platform.h"
#include "iroha/unicode.h"
#include "selection_app.h"

#ifndef IROHA_VERSION
#define IROHA_VERSION "dev"
#endif

namespace {

// 記録のファイルがこれを超えていたら、起動のときに 1 世代だけ残して作り直す
constexpr uintmax_t kMaxLogBytes = 1 << 20;

// 動作の記録: <データフォルダ>/logs/selection-<ホスト名>.log と標準エラー（裏のスレッドからも書く）
class FileLog {
public:
    FileLog() {
        const std::filesystem::path directory = iroha::platform::DataDirectory() / "logs";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        path_ = directory / iroha::PathFromUtf8("selection-" + iroha::platform::HostName() + ".log");
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

void PrintUsage(const char* program) {
    std::printf(
        "使い方: %s [--help] [--version]\n"
        "\n"
        "選択テキストのAI編集の常駐プログラム（X11）。ほかのアプリで選んだ文章（PRIMARY 選択）を、\n"
        "ショートカットで AI に渡し、結果の窓から元のアプリに貼って置き換えます。\n"
        "設定は <データフォルダ>/config.json の selection* と AI の接続先（設定画面で変える）。\n"
        "selectionEnabled が偽の間はショートカットをつかみません。Wayland のセッションでは何もしません。\n"
        "すでに動いていれば、新しく起動したものはすぐに終わります。\n"
        "\n"
        "データフォルダ: %s\n"
        "記録:           %s\n",
        program, iroha::PathToUtf8(iroha::platform::DataDirectory()).c_str(),
        iroha::PathToUtf8(iroha::platform::DataDirectory() / "logs" /
                          iroha::PathFromUtf8("selection-" + iroha::platform::HostName() + ".log"))
            .c_str());
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            PrintUsage(argv[0]);
            return 0;
        }
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("iroha-selection %s\n", IROHA_VERSION);
            return 0;
        }
        std::fprintf(stderr, "使い方: %s [--help] [--version]\n", argv[0]);
        return 2;
    }

    FileLog fileLog;
    const auto log = [&fileLog](const std::string& message) { fileLog.Write(message); };
    // 画面が無ければ GTK を初期化できない（GTK は初期化に失敗するとその場で終わるので、先に確かめる）
    if (!iroha::platform::GetEnv("DISPLAY") && !iroha::platform::GetEnv("WAYLAND_DISPLAY")) {
        log("no display (DISPLAY is not set)");
        return 1;
    }
    log(std::string("start ") + IROHA_VERSION);
    return iroha::selection::RunApplication(log);
}
