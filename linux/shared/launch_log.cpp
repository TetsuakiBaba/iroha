#include "launch_log.h"

#include <signal.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

#include "iroha/json.h"
#include "iroha/unicode.h"

namespace iroha {

namespace {

int64_t Now() { return static_cast<int64_t>(std::time(nullptr)); }

std::string Format(int64_t time) {
    const std::time_t t = static_cast<std::time_t>(time);
    std::tm local{};
    localtime_r(&t, &local);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    return text;
}

struct State {
    long pid = 0;
    int64_t startedAt = 0;
    std::string version;
    int64_t reportsCheckedAt = 0;
    int64_t exitedAt = 0; // 0 なら実行中か異常終了
    std::string exitReason;
};

bool ReadState(const std::filesystem::path& path, State* state) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    json::Value root;
    if (!json::Parse(buffer.str(), &root)) return false;
    const auto number = [&root](const char* key) -> int64_t {
        const json::Value* value = root.Find(key);
        return value && value->type == json::Value::Type::Number ? static_cast<int64_t>(value->number) : 0;
    };
    const auto text = [&root](const char* key) -> std::string {
        const json::Value* value = root.Find(key);
        return value ? value->AsString() : "";
    };
    state->pid = static_cast<long>(number("pid"));
    state->startedAt = number("startedAt");
    state->version = text("version");
    state->reportsCheckedAt = number("reportsCheckedAt");
    state->exitedAt = number("exitedAt");
    state->exitReason = text("exitReason");
    return state->pid != 0;
}

void WriteState(const std::filesystem::path& path, const State& state) {
    std::string text = "{\"pid\":" + std::to_string(state.pid) + ",\"startedAt\":" + std::to_string(state.startedAt) +
                       ",\"version\":" + json::QuoteString(state.version) +
                       ",\"reportsCheckedAt\":" + std::to_string(state.reportsCheckedAt);
    if (state.exitedAt != 0) {
        text += ",\"exitedAt\":" + std::to_string(state.exitedAt) + ",\"exitReason\":" + json::QuoteString(state.exitReason);
    }
    text += "}\n";
    const auto temporary = TemporaryPath(path);
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << text;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
}

bool IsProcessAlive(long pid) { return pid > 0 && (kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM); }

std::string OsVersion() {
    std::ifstream file("/etc/os-release");
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            std::string value = line.substr(12);
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            return value;
        }
    }
    utsname name{};
    return uname(&name) == 0 ? std::string(name.sysname) + " " + name.release : "Linux";
}

int64_t ModifiedTime(const std::filesystem::path& path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    if (error) return 0;
    const auto system = std::chrono::time_point_cast<std::chrono::seconds>(
        time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return static_cast<int64_t>(system.time_since_epoch().count());
}

} // namespace

LaunchLog::LaunchLog(std::filesystem::path directory, std::string hostName, std::string name)
    : directory_(std::move(directory)), hostName_(std::move(hostName)), name_(std::move(name)) {}

std::filesystem::path LaunchLog::LogPath() const { return directory_ / PathFromUtf8("launch-" + hostName_ + ".log"); }

std::filesystem::path LaunchLog::StatePath() const { return directory_ / PathFromUtf8("state-" + hostName_ + ".json"); }

std::vector<std::string> LaunchLog::RecordLaunch(const std::string& version, const std::filesystem::path& crashDirectory) {
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    const int64_t now = Now();
    const long pid = static_cast<long>(getpid());
    std::vector<std::string> lines;
    State previous;
    const bool hasPrevious = ReadState(StatePath(), &previous);
    if (hasPrevious && previous.exitedAt == 0) {
        if (previous.pid != pid && IsProcessAlive(previous.pid)) {
            lines.push_back("note 別のエンジンが動作中 pid=" + std::to_string(previous.pid) + " 開始=" +
                            Format(previous.startedAt) + " version=" + previous.version);
        } else {
            lines.push_back("unclean-exit 前回の終了が記録されていない（クラッシュ・強制終了・シャットダウンなど） pid=" +
                            std::to_string(previous.pid) + " 開始=" + Format(previous.startedAt) +
                            " version=" + previous.version);
        }
    }
    // apport のレポート（/var/crash/_usr_local_lib_ibus-iroha_ibus-engine-iroha.<uid>.crash など）
    const int64_t since = hasPrevious ? previous.reportsCheckedAt : now - 7 * 24 * 60 * 60;
    for (std::filesystem::directory_iterator it(crashDirectory, error), end; !error && it != end; it.increment(error)) {
        const std::string file = it->path().filename().string();
        if (file.find(name_) == std::string::npos || it->path().extension() != ".crash") continue;
        const int64_t modified = ModifiedTime(it->path());
        if (modified > since) lines.push_back("crash-report " + it->path().string() + " 時刻=" + Format(modified));
    }
    lines.push_back("launch pid=" + std::to_string(pid) + " version=" + version + " os=" + OsVersion() +
                    " host=" + hostName_);
    State state;
    state.pid = pid;
    state.startedAt = now;
    state.version = version;
    state.reportsCheckedAt = now;
    WriteState(StatePath(), state);
    Append(lines);
    return lines;
}

void LaunchLog::RecordExit(const std::string& reason) {
    const long pid = static_cast<long>(getpid());
    Append({"exit reason=" + reason + " pid=" + std::to_string(pid)});
    State state;
    if (ReadState(StatePath(), &state) && state.pid == pid) {
        state.exitedAt = Now();
        state.exitReason = reason;
        WriteState(StatePath(), state);
    }
}

void LaunchLog::Append(const std::vector<std::string>& lines) {
    std::vector<std::string> all;
    {
        std::ifstream file(LogPath(), std::ios::binary);
        std::string line;
        while (std::getline(file, line)) all.push_back(line);
    }
    const std::string stamp = Format(Now());
    for (const std::string& line : lines) all.push_back(stamp + " " + line);
    if (all.size() > kMaxLines) all.erase(all.begin(), all.begin() + static_cast<long>(all.size() - kMaxLines / 2));
    const auto temporary = TemporaryPath(LogPath());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        for (const std::string& line : all) file << line << '\n';
    }
    std::error_code error;
    std::filesystem::rename(temporary, LogPath(), error);
}

} // namespace iroha
