#include "iroha/config.h"

#include <fstream>
#include <sstream>

#include "iroha/json.h"
#include "iroha/unicode.h"

namespace iroha {

Config LoadConfig(const std::filesystem::path& path) {
    Config config;
    std::ifstream file(path, std::ios::binary);
    if (!file) return config;
    std::stringstream buffer;
    buffer << file.rdbuf();
    json::Value root;
    if (!json::Parse(buffer.str(), &root)) return config;
    if (const json::Value* model = root.Find("model")) {
        config.model = Utf8ToUtf32(model->AsString());
    }
    return config;
}

bool SaveConfig(const std::filesystem::path& path, const Config& config) {
    std::string out = "{\"model\":" + json::QuoteString(Utf32ToUtf8(config.model)) +
                      ",\"version\":1}";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path tmp = path.wstring() + L".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(out.data(), static_cast<std::streamsize>(out.size()));
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

} // namespace iroha
