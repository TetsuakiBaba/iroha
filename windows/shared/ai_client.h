#pragma once
// AI変換の通信（Windows 専用。WinHTTP）。Ollama・LM Studio・OpenAI互換・Claude にストリーミングで依頼し、
// 途中経過（累積の出力）を渡しながら完成した文を返す。
// 移植元: macos/Sources/iroha/RemoteTranslator.swift（依頼の中身・思考を止める指定・応答の読み方を合わせること）

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "iroha/ai_conversion.h"

namespace iroha {

// 接続先 1 つ分（変換サーバが config.json と資格情報マネージャーから作る）
struct AIConnection {
    AIService service = AIService::None;
    std::string endpoint; // 空なら既定（Ollama http://localhost:11434 など）
    std::string model;
    std::string apiKey; // OpenAI互換（無くてもよい）・Claude（必須）
};

struct AIResult {
    std::optional<std::u32string> text; // 失敗したら無し
    std::string error;                  // 失敗の理由（HTTP の番号とサーバのエラー文・接続できない理由など）
};

class AIClient {
public:
    // Claude の既定のモデル（macOS版と同じ。AI変換は短い文の書き換えが多いので、速くて安いもの）
    static constexpr const char* kClaudeDefaultModel = "claude-haiku-5-5";
    // API キーを資格情報マネージャーに置く名前（macOS版のキーチェーンの名前と同じ）
    static constexpr const char* kOpenAIKeyAccount = "openAIAPIKey";
    static constexpr const char* kClaudeKeyAccount = "anthropicAPIKey";

    // 依頼する。stallSeconds の間サーバから何も届かなければ打ち切る（進みがある限りは打ち切らない）。
    // cancel を止めると、通信の途中でも戻る
    static AIResult Run(const AIConnection& connection, const AIRequest& request,
                        const std::function<void(const std::u32string&)>& onPartial, CancelToken* cancel,
                        int stallSeconds = 30);

    // 使えるモデルの一覧（設定画面用）
    static bool ListModels(const AIConnection& connection, std::vector<std::string>* models, std::string* error);

    // 使える状態か（モデルが決まっている・Claude なら API キーがある）
    static bool IsConfigured(const AIConnection& connection);

    // ベース URL の表記ゆれ（末尾のスラッシュ・/v1 の有無）を吸収して "…/v1" にそろえる
    static std::string NormalizedV1Base(const std::string& endpoint);
    static std::string DefaultEndpoint(AIService service);
};

} // namespace iroha
