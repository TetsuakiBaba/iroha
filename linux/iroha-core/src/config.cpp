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
// 選択テキストのAI編集のプリセット（書いてある項目だけ）
std::string EncodeSelectionPresets(const Config& config) {
    std::string out;
    for (size_t i = 0; i < config.selectionPresets.size(); ++i) {
        const std::string prefix = ",\"selPreset" + std::to_string(i);
        const Config::SelectionPreset& preset = config.selectionPresets[i];
        if (preset.name) out += prefix + "Name\":" + json::QuoteString(Utf32ToUtf8(*preset.name));
        if (preset.prompt) out += prefix + "Prompt\":" + json::QuoteString(Utf32ToUtf8(*preset.prompt));
        if (preset.hotkey) out += prefix + "Hotkey\":" + json::QuoteString(*preset.hotkey);
        if (preset.enabled) out += prefix + "Enabled\":" + (*preset.enabled ? "true" : "false");
    }
    return out;
}

// AI変換のプリセット（書いてある項目だけ）
std::string EncodePresets(const Config& config) {
    std::string out;
    for (size_t i = 0; i < config.aiPresets.size(); ++i) {
        const std::string prefix = ",\"aiPreset" + std::to_string(i);
        const Config::AIPreset& preset = config.aiPresets[i];
        if (preset.name) out += prefix + "Name\":" + json::QuoteString(Utf32ToUtf8(*preset.name));
        if (preset.prompt) out += prefix + "Prompt\":" + json::QuoteString(Utf32ToUtf8(*preset.prompt));
        if (preset.hotkey) out += prefix + "Hotkey\":" + json::QuoteString(*preset.hotkey);
    }
    return out;
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
    for (const auto& flag : {std::make_pair("learningEnabled", &config.learningEnabled),
                             std::make_pair("documentContext", &config.documentContext),
                             std::make_pair("developerMode", &config.developerMode)}) {
        if (const json::Value* value = root.Find(flag.first); value && value->type == json::Value::Type::Bool) {
            *flag.second = value->boolean;
        }
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
    const auto text = [&root](const char* key, std::string* out) {
        if (const json::Value* value = root.Find(key); value && value->type == json::Value::Type::String) {
            *out = value->string;
        }
    };
    text("translationService", &config.aiService);
    text("ollamaEndpoint", &config.ollamaEndpoint);
    text("ollamaModel", &config.ollamaModel);
    text("lmStudioEndpoint", &config.lmStudioEndpoint);
    text("lmStudioModel", &config.lmStudioModel);
    text("openAIEndpoint", &config.openAIEndpoint);
    text("openAIModel", &config.openAIModel);
    text("claudeModel", &config.claudeModel);
    for (size_t i = 0; i < config.aiPresets.size(); ++i) {
        const std::string prefix = "aiPreset" + std::to_string(i);
        if (const json::Value* value = root.Find(prefix + "Name"); value && value->type == json::Value::Type::String) {
            config.aiPresets[i].name = Utf8ToUtf32(value->string);
        }
        if (const json::Value* value = root.Find(prefix + "Prompt"); value && value->type == json::Value::Type::String) {
            config.aiPresets[i].prompt = Utf8ToUtf32(value->string);
        }
        if (const json::Value* value = root.Find(prefix + "Hotkey"); value && value->type == json::Value::Type::String) {
            config.aiPresets[i].hotkey = value->string;
        }
    }
    if (const json::Value* delimited = root.Find("delimitedAlphabet");
        delimited && delimited->type == json::Value::Type::Bool) {
        config.delimitedAlphabet = delimited->boolean;
    }
    text("delimitedAlphabetStart", &config.delimitedAlphabetStart);
    text("delimitedAlphabetEnd", &config.delimitedAlphabetEnd);
    if (const json::Value* fix = root.Find("aiFix"); fix && fix->type == json::Value::Type::Bool) {
        config.aiFix = fix->boolean;
    }
    if (const json::Value* threshold = root.Find("aiFixThreshold");
        threshold && threshold->type == json::Value::Type::Number && std::isfinite(threshold->number)) {
        config.aiFixThreshold = std::max(0.0, std::min(20.0, threshold->number));
    }
    text("aiFixShortcut", &config.aiFixShortcut);
    if (const json::Value* selection = root.Find("selectionEnabled");
        selection && selection->type == json::Value::Type::Bool) {
        config.selectionEnabled = selection->boolean;
    }
    if (const json::Value* hotkey = root.Find("selectionOnDemandHotkey");
        hotkey && hotkey->type == json::Value::Type::String) {
        config.selectionOnDemandHotkey = hotkey->string;
    }
    text("selectionExcludedApps", &config.selectionExcludedApps);
    text("selectionTriggerMode", &config.selectionTriggerMode);
    text("privateModeHotkey", &config.privateModeHotkey);
    text("modelAdapterPath", &config.modelAdapterPath);
    if (const json::Value* count = root.Find("selectionCharacterCount");
        count && count->type == json::Value::Type::Bool) {
        config.selectionCharacterCount = count->boolean;
    }
    for (size_t i = 0; i < config.selectionPresets.size(); ++i) {
        const std::string prefix = "selPreset" + std::to_string(i);
        auto& preset = config.selectionPresets[i];
        if (const json::Value* value = root.Find(prefix + "Name"); value && value->type == json::Value::Type::String) {
            preset.name = Utf8ToUtf32(value->string);
        }
        if (const json::Value* value = root.Find(prefix + "Prompt"); value && value->type == json::Value::Type::String) {
            preset.prompt = Utf8ToUtf32(value->string);
        }
        if (const json::Value* value = root.Find(prefix + "Hotkey"); value && value->type == json::Value::Type::String) {
            preset.hotkey = value->string;
        }
        if (const json::Value* value = root.Find(prefix + "Enabled"); value && value->type == json::Value::Type::Bool) {
            preset.enabled = value->boolean;
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
                      ",\"privateModeHotkey\":" + json::QuoteString(config.privateModeHotkey) +
                      ",\"learningEnabled\":" + (config.learningEnabled ? "true" : "false") +
                      ",\"documentContext\":" + (config.documentContext ? "true" : "false") +
                      ",\"modelAdapterPath\":" + json::QuoteString(config.modelAdapterPath) +
                      ",\"developerMode\":" + (config.developerMode ? "true" : "false") +
                      ",\"conversionLogEnabled\":" + (config.conversionLog ? "true" : "false") +
                      ",\"conversionLogScope\":" + (config.conversionLogCorrectionsOnly ? "\"corrections\"" : "\"all\"") +
                      ",\"typoNormalizer\":" + (config.typoCorrection ? "true" : "false") +
                      ",\"typoNormalizerThreshold\":" + FormatNumber(config.typoThreshold) +
                      ",\"typoNormalizerDelayMs\":" + std::to_string(config.typoDelayMs) +
                      ",\"typoNormalizerMinLength\":" + std::to_string(config.typoMinimumLength) +
                      ",\"translationService\":" + json::QuoteString(config.aiService) +
                      ",\"ollamaEndpoint\":" + json::QuoteString(config.ollamaEndpoint) +
                      ",\"ollamaModel\":" + json::QuoteString(config.ollamaModel) +
                      ",\"lmStudioEndpoint\":" + json::QuoteString(config.lmStudioEndpoint) +
                      ",\"lmStudioModel\":" + json::QuoteString(config.lmStudioModel) +
                      ",\"openAIEndpoint\":" + json::QuoteString(config.openAIEndpoint) +
                      ",\"openAIModel\":" + json::QuoteString(config.openAIModel) +
                      ",\"claudeModel\":" + json::QuoteString(config.claudeModel) + EncodePresets(config) +
                      ",\"delimitedAlphabet\":" + (config.delimitedAlphabet ? "true" : "false") +
                      ",\"delimitedAlphabetStart\":" + json::QuoteString(config.delimitedAlphabetStart) +
                      ",\"delimitedAlphabetEnd\":" + json::QuoteString(config.delimitedAlphabetEnd) +
                      ",\"aiFix\":" + (config.aiFix ? "true" : "false") +
                      ",\"aiFixThreshold\":" + FormatNumber(config.aiFixThreshold) +
                      ",\"aiFixShortcut\":" + json::QuoteString(config.aiFixShortcut) +
                      ",\"selectionEnabled\":" + (config.selectionEnabled ? "true" : "false") +
                      (config.selectionOnDemandHotkey
                           ? ",\"selectionOnDemandHotkey\":" + json::QuoteString(*config.selectionOnDemandHotkey)
                           : std::string()) +
                      ",\"selectionExcludedApps\":" + json::QuoteString(config.selectionExcludedApps) +
                      ",\"selectionTriggerMode\":" + json::QuoteString(config.selectionTriggerMode) +
                      ",\"selectionCharacterCount\":" + (config.selectionCharacterCount ? "true" : "false") +
                      EncodeSelectionPresets(config) +
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
