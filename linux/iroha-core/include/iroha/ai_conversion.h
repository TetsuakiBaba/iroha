#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace iroha {

// AI変換（未確定文字列をプリセットのプロンプトで AI に渡す）のうち、通信を含まない部分。
// 移植元: macos/Sources/iroha/AICommitPreset.swift・TranslationService.swift・RemoteTranslator.swift・
// ClaudeStreamReader.swift（依頼の組み立てと応答の読み方を合わせること。通信は windows/shared/ai_client）

// AI の接続先（macOS版の TranslationBackend から Apple Intelligence を除いたもの）
enum class AIService {
    None,     // 設定していない（AI変換のキーでは普通に確定する）
    Ollama,   // ローカルの Ollama
    LMStudio, // ローカルの LM Studio
    OpenAI,   // OpenAI 互換 API（API キーは資格情報マネージャー。テキストが外部に送られる）
    Claude,   // Claude（Anthropic の Messages API。API キーは資格情報マネージャー）
};
// 設定の値（macOS版の translationService と同じ "ollama" / "lmstudio" / "openai" / "claude"）
AIService ParseAIService(const std::string& value);
const char* AIServiceKey(AIService service);
std::u32string AIServiceDisplayName(AIService service);

// AI に依頼した結果（失敗なら text が無く、error に理由）
struct AIOutcome {
    std::optional<std::u32string> text;
    std::string error;
};

// AI への 1 回の依頼（指示 + 入力テキスト）
struct AIRequest {
    std::u32string instructions;
    std::u32string userMessage;

    // プロンプトに {text} があればその位置へ差し込み、無ければ指示のあとに続けて渡す
    static AIRequest Build(const std::u32string& prompt, const std::u32string& text);
};

// AI変換のプリセット 1 つ分（名前・プロンプト・ショートカット）。英訳もユーザ定義の変換も、
// AI に違うプロンプトを渡しているだけなので同じ形で 3 つ持つ
struct AICommitPreset {
    std::u32string name;   // 空なら既定の名前
    std::u32string prompt; // 空なら既定のプロンプト
    std::string shortcut;  // "Alt+Enter" の形（空なら割り当てなし）

    std::u32string DisplayName(size_t index) const;
    std::u32string EffectivePrompt(size_t index) const;
    AIRequest Request(size_t index, const std::u32string& text) const;
};

namespace AICommitSettings {
constexpr size_t kCount = 3;
// プロンプトの中で入力テキストの位置を示す差し込み記号
extern const char32_t kTextPlaceholder[];
// 出力を 1 本のテキストに絞るための念押し（プロンプトの末尾に足す）
extern const char32_t kOutputRule[];
// 英訳のプロンプト（macOS版 TranslationService.translateInstructions）
extern const char32_t kTranslateInstructions[];
struct Default {
    const char32_t* name;
    const char32_t* prompt;
    const char* shortcut; // Windows の既定（英訳は Shift+Space。macOS版は Option+Return。2026-11-14 ユーザの指定）
};
const std::vector<Default>& Defaults();
} // namespace AICommitSettings

// 選択テキストのAI編集のプリセット（macOS版 SelectionSettings.defaults）。ショートカットは Windows の既定
// （macOS版の Ctrl+1〜4 は Windows ではブラウザのタブ切り替えなどと重なるので Ctrl+Alt+1〜4）
namespace SelectionSettingsDefaults {
constexpr size_t kCount = 5;
struct Preset {
    const char32_t* name;
    const char32_t* prompt;
    const char* hotkey;
    bool enabled;
};
const std::vector<Preset>& Presets();
// その場でプロンプトを入れるショートカット（macOS版の Ctrl+0 にあたる）
constexpr const char* kOnDemandHotkey = "Ctrl+Alt+0";
// 選択が無いときの「テキストを生成」の指示
extern const char32_t kGenerationInstructions[];
} // namespace SelectionSettingsDefaults

// 選択テキストのAI編集の出力の軽い整形（macOS版 AIOutputCleaner）: <think> を除き、全体を包むコードの囲み・
// 1 組の引用符・括弧を外す（中に同じ記号が無いときだけ）
std::u32string CleanAIOutput(const std::u32string& text);

// ひらがな・カタカナ・漢字を 1 文字でも含むか（AI の結果を確定せずに文節で返すかの判定）
bool ContainsJapanese(const std::u32string& text);

// <think>…</think> を除く（思考を止める指定をすり抜けた場合の保険）。閉じタグがまだなら、そこまでの本文だけ
std::string StripThinking(const std::string& text);

// エラー応答の本文からメッセージを取り出す（{"error":{"message":…}} と {"error":"…"}）。JSON でなければ本文の先頭
std::optional<std::string> AIErrorMessageFromBody(const std::string& body);

// OpenAI 互換の SSE の 1 行から本文の差分を取り出す（"data: [DONE]" なら done）
struct OpenAIStreamLine {
    std::string piece;
    bool done = false;
};
OpenAIStreamLine ParseOpenAIStreamLine(const std::string& line);

// Ollama の /api/chat の JSONL の 1 行
struct OllamaStreamLine {
    std::string piece;
    bool done = false;
    std::optional<std::string> error;
};
OllamaStreamLine ParseOllamaStreamLine(const std::string& line);

// Claude の Messages API のストリーム（SSE）を 1 行ずつ読み、本文と終わりの理由を集める（macOS版 ClaudeStreamReader）
class ClaudeStreamReader {
public:
    // 1 行を読む。本文が増えたら真。エラーの行なら error に理由を入れて偽（Failed() が真になる）
    bool Consume(const std::string& line);
    const std::string& Text() const { return text_; }
    bool IsFinished() const { return finished_; }
    bool Failed() const { return failure_.has_value(); }
    // 読み終えたあとの結果。断られていた・エラーなら無しで、FailureMessage() に理由
    std::optional<std::string> Result() const;
    std::string FailureMessage() const;

private:
    std::string text_;
    std::optional<std::string> stopReason_;
    std::optional<std::string> refusalCategory_;
    std::optional<std::string> failure_;
    bool finished_ = false;
};

// 進行中の依頼を止める合図。通信を待っている間でも止められるよう、止め方（通信の取っ手を閉じる）を登録できる
class CancelToken {
public:
    void Cancel() {
        std::function<void()> canceller;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_) return;
            cancelled_ = true;
            canceller = canceller_;
        }
        if (canceller) canceller();
    }
    bool Cancelled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cancelled_;
    }
    // 止めたときに呼ぶものを登録する（nullptr で外す）。既に止めてあれば偽を返し、登録しない
    bool SetCanceller(std::function<void()> canceller) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_ && canceller) return false;
        canceller_ = std::move(canceller);
        return true;
    }

private:
    mutable std::mutex mutex_;
    bool cancelled_ = false;
    std::function<void()> canceller_;
};

} // namespace iroha
