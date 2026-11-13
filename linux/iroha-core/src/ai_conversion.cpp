#include "iroha/ai_conversion.h"

#include "iroha/json.h"
#include "iroha/unicode.h"

namespace iroha {

namespace {

std::string TrimAscii(const std::string& text) {
    size_t start = 0;
    size_t end = text.size();
    while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n')) {
        ++start;
    }
    while (end > start &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return text.substr(start, end - start);
}

} // namespace

AIService ParseAIService(const std::string& value) {
    if (value == "ollama") return AIService::Ollama;
    if (value == "lmstudio") return AIService::LMStudio;
    if (value == "openai") return AIService::OpenAI;
    if (value == "claude") return AIService::Claude;
    return AIService::None;
}

const char* AIServiceKey(AIService service) {
    switch (service) {
    case AIService::Ollama:
        return "ollama";
    case AIService::LMStudio:
        return "lmstudio";
    case AIService::OpenAI:
        return "openai";
    case AIService::Claude:
        return "claude";
    default:
        return "";
    }
}

std::u32string AIServiceDisplayName(AIService service) {
    switch (service) {
    case AIService::Ollama:
        return U"Ollama";
    case AIService::LMStudio:
        return U"LM Studio";
    case AIService::OpenAI:
        return U"OpenAI互換";
    case AIService::Claude:
        return U"Claude";
    default:
        return U"使わない";
    }
}

namespace AICommitSettings {
const char32_t kTextPlaceholder[] = U"{text}";
const char32_t kOutputRule[] = U"出力は変換後のテキストのみ。説明・注釈・引用符・前置きを付けないこと。";
const char32_t kTranslateInstructions[] =
    U"You are a Japanese-to-English translation engine. "
    U"Translate the user's Japanese text into natural English. "
    U"Output ONLY the English translation - no quotation marks, no romaji, "
    U"no notes, no explanations, no alternatives.";

const std::vector<Default>& Defaults() {
    static const std::vector<Default> defaults = {
        {U"英訳", kTranslateInstructions, "Alt+Enter"},
        {U"敬語", U"次の日本語を、意味を変えずに丁寧なビジネス文体に書き直してください。", ""},
        {U"要約", U"次の日本語を、要点を保ったまま短く言い換えてください。", ""},
    };
    return defaults;
}
} // namespace AICommitSettings

AIRequest AIRequest::Build(const std::u32string& prompt, const std::u32string& text) {
    const std::u32string placeholder = AICommitSettings::kTextPlaceholder;
    const size_t at = prompt.find(placeholder);
    if (at != std::u32string::npos) {
        std::u32string message = prompt;
        for (size_t position = message.find(placeholder); position != std::u32string::npos;
             position = message.find(placeholder, position + text.size())) {
            message.replace(position, placeholder.size(), text);
        }
        return AIRequest{AICommitSettings::kOutputRule, message};
    }
    return AIRequest{prompt + U"\n" + AICommitSettings::kOutputRule, text};
}

std::u32string AICommitPreset::DisplayName(size_t index) const {
    const std::u32string trimmed = TrimWhitespace(name);
    if (!trimmed.empty() || index >= AICommitSettings::Defaults().size()) return trimmed;
    return AICommitSettings::Defaults()[index].name;
}

std::u32string AICommitPreset::EffectivePrompt(size_t index) const {
    const std::u32string trimmed = TrimWhitespace(prompt);
    if (!trimmed.empty() || index >= AICommitSettings::Defaults().size()) return trimmed;
    return AICommitSettings::Defaults()[index].prompt;
}

AIRequest AICommitPreset::Request(size_t index, const std::u32string& text) const {
    return AIRequest::Build(EffectivePrompt(index), text);
}

namespace SelectionSettingsDefaults {
const char32_t kGenerationInstructions[] = U"ユーザの指示に従ってテキストを作成してください。\n";

const std::vector<Preset>& Presets() {
    static const std::vector<Preset> presets = {
        {U"日英翻訳", U"次のテキストを翻訳してください。日本語なら英語に、英語なら日本語に訳してください。", "Ctrl+Alt+1",
         true},
        {U"校正", U"次のテキストの誤字脱字と不自然な表現を直してください。意味と文体は変えないでください。", "Ctrl+Alt+2",
         true},
        {U"敬語", U"次の日本語を、意味を変えずに丁寧なビジネス文体に書き直してください。", "Ctrl+Alt+3", true},
        {U"要約", U"次のテキストを、要点を保ったまま短く言い換えてください。", "Ctrl+Alt+4", true},
        {U"カスタム", U"", "", false},
    };
    return presets;
}
} // namespace SelectionSettingsDefaults

std::u32string CleanAIOutput(const std::u32string& text) {
    std::u32string cleaned = TrimWhitespace(Utf8ToUtf32(StripThinking(Utf32ToUtf8(text))));
    // 全体が 1 つのコードの囲みに包まれていたら中身だけにする
    const std::u32string fence = U"```";
    if (cleaned.size() > 6 && cleaned.compare(0, 3, fence) == 0 && cleaned.compare(cleaned.size() - 3, 3, fence) == 0) {
        std::u32string inner = cleaned.substr(3, cleaned.size() - 6);
        // 開きの直後の言語名（```text など）は最初の改行まで捨てる
        const size_t newline = inner.find(U'\n');
        if (newline != std::u32string::npos) {
            const std::u32string firstLine = inner.substr(0, newline);
            if (firstLine.find(U' ') == std::u32string::npos && firstLine.size() <= 20) inner = inner.substr(newline + 1);
        }
        const std::u32string candidate = TrimWhitespace(inner);
        if (!candidate.empty()) cleaned = candidate;
    }
    // 全体が 1 組の引用符・括弧に包まれていたら外す
    const std::pair<char32_t, char32_t> quotes[] = {
        {U'「', U'」'}, {U'『', U'』'}, {U'\u201C', U'\u201D'}, {U'"', U'"'}, {U'\'', U'\''}};
    for (const auto& [open, close] : quotes) {
        if (cleaned.size() >= 2 && cleaned.front() == open && cleaned.back() == close) {
            const std::u32string inner = cleaned.substr(1, cleaned.size() - 2);
            if (inner.find(open) == std::u32string::npos && inner.find(close) == std::u32string::npos && !inner.empty()) {
                cleaned = TrimWhitespace(inner);
            }
            break;
        }
    }
    return cleaned;
}

bool ContainsJapanese(const std::u32string& text) {
    for (char32_t c : text) {
        if ((c >= 0x3040 && c <= 0x30FF) ||   // ひらがな・カタカナ
            (c >= 0x31F0 && c <= 0x31FF) ||   // カタカナ拡張
            (c >= 0x3400 && c <= 0x4DBF) ||   // CJK統合漢字拡張A
            (c >= 0x4E00 && c <= 0x9FFF) ||   // CJK統合漢字
            (c >= 0xF900 && c <= 0xFAFF) ||   // CJK互換漢字
            (c >= 0xFF66 && c <= 0xFF9F) ||   // 半角カタカナ
            (c >= 0x20000 && c <= 0x2FA1F)) { // CJK統合漢字拡張B以降・互換漢字補助
            return true;
        }
    }
    return false;
}

std::string StripThinking(const std::string& text) {
    std::string result = text;
    for (size_t start = result.find("<think>"); start != std::string::npos; start = result.find("<think>")) {
        const size_t end = result.find("</think>", start + 7);
        if (end == std::string::npos) {
            result.resize(start);
            break;
        }
        result.erase(start, end + 8 - start);
    }
    return TrimAscii(result);
}

std::optional<std::string> AIErrorMessageFromBody(const std::string& body) {
    const std::string trimmed = TrimAscii(body);
    if (trimmed.empty()) return std::nullopt;
    json::Value root;
    if (json::Parse(trimmed, &root) && root.type == json::Value::Type::Object) {
        if (const json::Value* error = root.Find("error")) {
            if (error->type == json::Value::Type::Object) {
                if (const json::Value* message = error->Find("message"); message && !message->AsString().empty()) {
                    return message->AsString();
                }
            } else if (error->type == json::Value::Type::String) {
                return error->string;
            }
        }
    }
    // 本文の先頭（UTF-8 の文字の途中で切らない）
    const std::u32string text = Utf8ToUtf32(trimmed);
    return Utf32ToUtf8(text.substr(0, 300));
}

OpenAIStreamLine ParseOpenAIStreamLine(const std::string& line) {
    OpenAIStreamLine result;
    if (line.rfind("data: ", 0) != 0) return result;
    const std::string payload = TrimAscii(line.substr(6));
    if (payload == "[DONE]") {
        result.done = true;
        return result;
    }
    json::Value root;
    if (!json::Parse(payload, &root)) return result;
    const json::Value* choices = root.Find("choices");
    if (!choices || choices->type != json::Value::Type::Array || choices->array.empty()) return result;
    if (const json::Value* delta = choices->array.front().Find("delta")) {
        if (const json::Value* content = delta->Find("content")) result.piece = content->AsString();
    }
    return result;
}

OllamaStreamLine ParseOllamaStreamLine(const std::string& line) {
    OllamaStreamLine result;
    json::Value root;
    if (!json::Parse(line, &root) || root.type != json::Value::Type::Object) return result;
    if (const json::Value* error = root.Find("error"); error && error->type == json::Value::Type::String) {
        result.error = error->string;
    }
    if (const json::Value* message = root.Find("message")) {
        if (const json::Value* content = message->Find("content")) result.piece = content->AsString();
    }
    if (const json::Value* done = root.Find("done"); done && done->type == json::Value::Type::Bool) {
        result.done = done->boolean;
    }
    return result;
}

bool ClaudeStreamReader::Consume(const std::string& line) {
    if (line.rfind("data:", 0) != 0) return false;
    json::Value event;
    if (!json::Parse(TrimAscii(line.substr(5)), &event) || event.type != json::Value::Type::Object) return false;
    const std::string type = event.Find("type") ? event.Find("type")->AsString() : "";
    if (type == "content_block_delta") {
        const json::Value* delta = event.Find("delta");
        if (!delta || !delta->Find("type") || delta->Find("type")->AsString() != "text_delta") return false;
        const std::string piece = delta->Find("text") ? delta->Find("text")->AsString() : "";
        if (piece.empty()) return false;
        text_ += piece;
        return true;
    }
    if (type == "message_delta") {
        const json::Value* delta = event.Find("delta");
        if (delta && delta->Find("stop_reason") && delta->Find("stop_reason")->type == json::Value::Type::String) {
            stopReason_ = delta->Find("stop_reason")->string;
        }
        const json::Value* details = delta ? delta->Find("stop_details") : nullptr;
        if (!details) details = event.Find("stop_details");
        if (details && details->Find("category") && details->Find("category")->type == json::Value::Type::String) {
            refusalCategory_ = details->Find("category")->string;
        }
    } else if (type == "error") {
        const json::Value* error = event.Find("error");
        const json::Value* message = error ? error->Find("message") : nullptr;
        failure_ = message && !message->AsString().empty() ? message->AsString() : "Claude API のエラー";
    } else if (type == "message_stop") {
        finished_ = true;
    }
    return false;
}

std::optional<std::string> ClaudeStreamReader::Result() const {
    if (failure_ || stopReason_ == std::optional<std::string>("refusal")) return std::nullopt;
    return TrimAscii(text_);
}

std::string ClaudeStreamReader::FailureMessage() const {
    if (failure_) return *failure_;
    if (stopReason_ == std::optional<std::string>("refusal")) {
        return "Claude が応答を断りました" + (refusalCategory_ ? "（" + *refusalCategory_ + "）" : std::string());
    }
    return {};
}

} // namespace iroha
