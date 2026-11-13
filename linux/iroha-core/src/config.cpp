#include "iroha/config.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "iroha/json.h"
#include "iroha/unicode.h"

namespace iroha {

namespace {
// 小数を短く書く（2.5 → "2.5"）
std::string FormatNumber(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.6g", value);
    return text;
}
} // namespace

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
    if (const json::Value* typo = root.Find("typoNormalizer")) {
        if (typo->type == json::Value::Type::Bool) config.typoCorrection = typo->boolean;
    }
    if (const json::Value* threshold = root.Find("typoNormalizerThreshold")) {
        if (threshold->type == json::Value::Type::Number && std::isfinite(threshold->number)) {
            config.typoThreshold = std::max(0.0, std::min(10.0, threshold->number));
        }
    }
    if (const json::Value* delay = root.Find("typoNormalizerDelayMs")) {
        if (delay->type == json::Value::Type::Number && delay->number != 0) {
            config.typoDelayMs = std::max(100, std::min(2000, static_cast<int>(delay->number)));
        }
    }
    if (const json::Value* length = root.Find("typoNormalizerMinLength")) {
        if (length->type == json::Value::Type::Number) {
            config.typoMinimumLength = std::max(1, std::min(20, static_cast<int>(length->number)));
        }
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
                      ",\"typoNormalizer\":" + (config.typoCorrection ? "true" : "false") +
                      ",\"typoNormalizerThreshold\":" + FormatNumber(config.typoThreshold) +
                      ",\"typoNormalizerDelayMs\":" + std::to_string(config.typoDelayMs) +
                      ",\"typoNormalizerMinLength\":" + std::to_string(config.typoMinimumLength) +
                      ",\"version\":1}";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path tmp = TemporaryPath(path);
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(out.data(), static_cast<std::streamsize>(out.size()));
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

} // namespace iroha
