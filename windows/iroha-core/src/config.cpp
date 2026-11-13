#include "iroha/config.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "iroha/json.h"
#include "iroha/unicode.h"

namespace iroha {

std::filesystem::path DefaultModelPath(const std::filesystem::path& modelsDirectory) {
    const std::filesystem::path current = modelsDirectory / kDefaultModelFileName;
    const std::filesystem::path previous = modelsDirectory / kPreviousDefaultModelFileName;
    std::error_code ignored;
    if (!std::filesystem::exists(current, ignored) && std::filesystem::exists(previous, ignored)) {
        return previous;
    }
    return current;
}

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
    if (const json::Value* live = root.Find("liveConversion")) {
        if (live->type == json::Value::Type::Bool) config.liveConversion = live->boolean;
    }
    if (const json::Value* symbols = root.Find("symbolCandidates")) {
        if (symbols->type == json::Value::Type::Bool) config.symbolCandidates = symbols->boolean;
    }
    if (const json::Value* punctuation = root.Find("punctuation")) {
        const std::u32string value = Utf8ToUtf32(punctuation->AsString());
        if (value == U"、。" || value == U"，．") config.punctuation = value;
    }
    if (const json::Value* space = root.Find("fullWidthSpace")) {
        if (space->type == json::Value::Type::Bool) config.fullWidthSpace = space->boolean;
    }
    if (const json::Value* predictive = root.Find("predictive")) {
        if (predictive->type == json::Value::Type::Bool) config.predictive = predictive->boolean;
    }
    if (const json::Value* privateMode = root.Find("privateMode")) {
        if (privateMode->type == json::Value::Type::Bool) config.privateMode = privateMode->boolean;
    }
    if (const json::Value* log = root.Find("conversionLogEnabled")) {
        if (log->type == json::Value::Type::Bool) config.conversionLog = log->boolean;
    }
    if (const json::Value* scope = root.Find("conversionLogScope")) {
        config.conversionLogCorrectionsOnly = scope->AsString() == "corrections";
    }
    if (const json::Value* count = root.Find("candidateCount")) {
        if (count->type == json::Value::Type::Number) {
            config.candidateCount = std::max(3, std::min(16, static_cast<int>(count->number)));
        }
    }
    return config;
}

bool SaveConfig(const std::filesystem::path& path, const Config& config) {
    std::string out = "{\"model\":" + json::QuoteString(Utf32ToUtf8(config.model)) +
                      ",\"liveConversion\":" + (config.liveConversion ? "true" : "false") +
                      ",\"symbolCandidates\":" + (config.symbolCandidates ? "true" : "false") +
                      ",\"punctuation\":" + json::QuoteString(Utf32ToUtf8(config.punctuation)) +
                      ",\"fullWidthSpace\":" + (config.fullWidthSpace ? "true" : "false") +
                      ",\"candidateCount\":" + std::to_string(config.candidateCount) +
                      ",\"predictive\":" + (config.predictive ? "true" : "false") +
                      ",\"privateMode\":" + (config.privateMode ? "true" : "false") +
                      ",\"conversionLogEnabled\":" + (config.conversionLog ? "true" : "false") +
                      ",\"conversionLogScope\":" + (config.conversionLogCorrectionsOnly ? "\"corrections\"" : "\"all\"") +
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
