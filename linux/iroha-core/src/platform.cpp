#include "iroha/platform.h"

#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <cstring>

#include "iroha/config.h"
#include "iroha/unicode.h"

namespace iroha::platform {

std::optional<std::string> GetEnv(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return std::nullopt;
    return std::string(value);
}

bool EnvEquals(const char* name, const char* value) {
    const char* env = std::getenv(name);
    return env != nullptr && std::strcmp(env, value) == 0;
}

std::filesystem::path DataDirectory() {
    if (const auto env = GetEnv("IROHA_DATA_DIR")) return PathFromUtf8(*env);
    if (const auto xdg = GetEnv("XDG_DATA_HOME")) return PathFromUtf8(*xdg) / "iroha";
    if (const auto home = GetEnv("HOME")) return PathFromUtf8(*home) / ".local" / "share" / "iroha";
    return std::filesystem::path(".") / "iroha";
}

std::filesystem::path StorePath(const char* envName, const std::filesystem::path& relative) {
    if (envName != nullptr) {
        if (const auto env = GetEnv(envName)) return PathFromUtf8(*env);
    }
    return DataDirectory() / relative;
}

std::filesystem::path ExecutableDirectory() {
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) return {};
    return path.parent_path();
}

std::string HostName() {
    // macOS版の LaunchLog.currentHostName と同じ規則（末尾の ".local" を落とし、"/" を "_" にする）
    char name[HOST_NAME_MAX + 1] = {};
    if (gethostname(name, sizeof(name) - 1) != 0) return "unknown";
    std::string host(name);
    const std::string suffix = ".local";
    if (host.size() >= suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0) {
        host.resize(host.size() - suffix.size());
    }
    for (char& c : host) {
        if (c == '/') c = '_';
    }
    return host.empty() ? "unknown" : host;
}

std::string ResolveModelPath() {
    if (const auto env = GetEnv("IROHA_MODEL")) return *env;
    const std::filesystem::path data = DataDirectory();
    const Config config = LoadConfig(data / "config.json");
    if (!config.model.empty()) {
        const std::filesystem::path path = PathFromUtf8(Utf32ToUtf8(config.model));
        if (path.is_absolute()) return PathToUtf8(path);
        return PathToUtf8(data / "models" / path);
    }
    return PathToUtf8(DefaultModelPath(data / "models"));
}

} // namespace iroha::platform
