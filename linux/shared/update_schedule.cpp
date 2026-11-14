#include "update_schedule.h"

#include <glib.h>

#include <chrono>
#include <vector>

#include "iroha/config.h"
#include "iroha/platform.h"
#include "iroha/unicode.h"
#include "iroha/update_check.h"

namespace iroha {

namespace {

// ログインの直後は避ける（Windows版と同じ）
constexpr auto kFirstDelay = std::chrono::minutes(2);
constexpr auto kInterval = std::chrono::hours(1);
constexpr int64_t kLaunchIntervalSeconds = 60 * 60;

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

UpdateSchedule::UpdateSchedule(std::filesystem::path settingsPath, std::function<void(const std::string&)> log)
    : settingsPath_(std::move(settingsPath)), log_(std::move(log)) {
    thread_ = std::thread([this] { Run(); });
}

UpdateSchedule::~UpdateSchedule() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool UpdateSchedule::ShouldLaunch(bool autoUpdateCheck, int64_t lastCheck, int64_t lastLaunch, int64_t now) {
    if (!autoUpdateCheck) return false;
    Update::State state;
    state.lastCheck = lastCheck;
    return Update::IsDue(state, now) && (lastLaunch == 0 || now - lastLaunch >= kLaunchIntervalSeconds || now < lastLaunch);
}

void UpdateSchedule::Run() {
    int64_t lastLaunch = 0;
    auto delay = std::chrono::duration_cast<std::chrono::seconds>(kFirstDelay);
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (wake_.wait_for(lock, delay, [this] { return stopping_; })) return;
        }
        delay = std::chrono::duration_cast<std::chrono::seconds>(kInterval);
        const std::filesystem::path data = platform::DataDirectory();
        const Config config = LoadConfig(data / "config.json");
        const Update::State state = Update::LoadState(Update::StatePath(data));
        const int64_t now = Now();
        if (!ShouldLaunch(config.autoUpdateCheck, state.lastCheck, lastLaunch, now)) continue;
        lastLaunch = now;
        // g_spawn_async は子を待たなくてよい形で起動する（ゾンビを残さない）。裏のスレッドから呼んでよい
        const std::string program = PathToUtf8(settingsPath_);
        std::vector<char*> argv = {const_cast<char*>(program.c_str()), const_cast<char*>("--check-update"), nullptr};
        GError* error = nullptr;
        if (g_spawn_async(nullptr, argv.data(), nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &error)) {
            if (log_) log_("update check started");
        } else {
            if (log_) log_(std::string("update check failed to start: ") + (error ? error->message : ""));
            if (error) g_error_free(error);
        }
    }
}

} // namespace iroha
