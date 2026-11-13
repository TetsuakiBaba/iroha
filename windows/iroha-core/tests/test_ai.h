#pragma once
// AI変換の、通信を含まない部分のテスト（依頼の組み立て・応答の読み方・ショートカットの文字列）。
// macOS版には対応するテストが無い（iroha 本体のターゲットはテストしていない）ので、Windows版で足したもの。
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include <string>
#include <vector>

#include "../../shared/key_shortcut.h"
#include "iroha/ai_conversion.h"

namespace {

void TestAIRequestBuild() {
    // {text} があればその位置へ差し込み、指示は出力の念押しだけにする
    auto request = iroha::AIRequest::Build(U"次を訳す: {text}", U"こんにちは");
    EXPECT_EQ(request.userMessage, U"次を訳す: こんにちは");
    EXPECT_EQ(request.instructions, iroha::AICommitSettings::kOutputRule);
    // 無ければ指示のあとに念押しを続け、テキストはそのまま渡す
    request = iroha::AIRequest::Build(U"丁寧にする", U"やっといて");
    EXPECT_EQ(request.userMessage, U"やっといて");
    EXPECT_EQ(request.instructions, std::u32string(U"丁寧にする\n") + iroha::AICommitSettings::kOutputRule);
    // プリセットの名前・プロンプトが空なら既定
    iroha::AICommitPreset preset;
    EXPECT_EQ(preset.DisplayName(0), U"英訳");
    EXPECT_EQ(preset.EffectivePrompt(1), U"次の日本語を、意味を変えずに丁寧なビジネス文体に書き直してください。");
    preset.name = U"  校正 ";
    EXPECT_EQ(preset.DisplayName(0), U"校正");
}

void TestAIResponseParsing() {
    EXPECT_TRUE(iroha::ContainsJapanese(U"Hello 世界") && iroha::ContainsJapanese(U"ｶﾅ"));
    EXPECT_TRUE(!iroha::ContainsJapanese(U"Hello, world."));
    EXPECT_TRUE(iroha::StripThinking("<think>考え中</think>\nHello ") == "Hello");
    EXPECT_TRUE(iroha::StripThinking("Hi<think>まだ") == "Hi");
    EXPECT_TRUE(iroha::AIErrorMessageFromBody(R"({"error":{"message":"invalid key"}})") ==
                std::optional<std::string>("invalid key"));
    EXPECT_TRUE(iroha::AIErrorMessageFromBody(R"({"error":"model not found"})") ==
                std::optional<std::string>("model not found"));
    EXPECT_TRUE(iroha::AIErrorMessageFromBody("Bad Gateway") == std::optional<std::string>("Bad Gateway"));
    EXPECT_TRUE(!iroha::AIErrorMessageFromBody("  "));

    auto openai = iroha::ParseOpenAIStreamLine(R"(data: {"choices":[{"delta":{"content":"Hel"}}]})");
    EXPECT_TRUE(openai.piece == "Hel" && !openai.done);
    EXPECT_TRUE(iroha::ParseOpenAIStreamLine("data: [DONE]").done);
    EXPECT_TRUE(iroha::ParseOpenAIStreamLine(": keep-alive").piece.empty());
    auto ollama = iroha::ParseOllamaStreamLine(R"({"message":{"content":"lo"},"done":false})");
    EXPECT_TRUE(ollama.piece == "lo" && !ollama.done);
    EXPECT_TRUE(iroha::ParseOllamaStreamLine(R"({"done":true})").done);
    EXPECT_TRUE(iroha::ParseOllamaStreamLine(R"({"error":"no model"})").error == std::optional<std::string>("no model"));

    // Claude: 本文は text_delta だけを足し、断られたら途中までの本文は使わない
    iroha::ClaudeStreamReader reader;
    EXPECT_TRUE(!reader.Consume("event: content_block_delta"));
    EXPECT_TRUE(reader.Consume(R"(data: {"type":"content_block_delta","delta":{"type":"text_delta","text":"Hello"}})"));
    EXPECT_TRUE(!reader.Consume(R"(data: {"type":"content_block_delta","delta":{"type":"thinking_delta","thinking":"…"}})"));
    reader.Consume(R"(data: {"type":"message_delta","delta":{"stop_reason":"end_turn"}})");
    reader.Consume(R"(data: {"type":"message_stop"})");
    EXPECT_TRUE(reader.IsFinished() && reader.Result() == std::optional<std::string>("Hello"));
    iroha::ClaudeStreamReader refused;
    refused.Consume(R"(data: {"type":"content_block_delta","delta":{"type":"text_delta","text":"途中"}})");
    refused.Consume(R"(data: {"type":"message_delta","delta":{"stop_reason":"refusal","stop_details":{"category":"cyber"}}})");
    EXPECT_TRUE(!refused.Result() && refused.FailureMessage() == "Claude が応答を断りました（cyber）");
    iroha::ClaudeStreamReader failed;
    failed.Consume(R"(data: {"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})");
    EXPECT_TRUE(failed.Failed() && failed.FailureMessage() == "Overloaded");

    iroha::CancelToken token;
    int closed = 0;
    EXPECT_TRUE(token.SetCanceller([&closed] { ++closed; }));
    token.Cancel();
    token.Cancel();
    EXPECT_TRUE(token.Cancelled() && closed == 1);
    EXPECT_TRUE(!token.SetCanceller([] {})); // 止めたあとは登録しない（すぐ戻る）
}

void TestKeyShortcut() {
    auto shortcut = iroha::KeyShortcut::Parse("Alt+Enter");
    EXPECT_TRUE(shortcut && shortcut->vk == 0x0D && shortcut->alt && !shortcut->control);
    shortcut = iroha::KeyShortcut::Parse("Option+Return"); // macOS版の書き方
    EXPECT_TRUE(shortcut && shortcut->vk == 0x0D && shortcut->alt);
    shortcut = iroha::KeyShortcut::Parse("shift + ctrl + t");
    EXPECT_TRUE(shortcut && shortcut->vk == 'T' && shortcut->shift && shortcut->control);
    EXPECT_TRUE(iroha::KeyShortcut::Parse("Ctrl+F5") && iroha::KeyShortcut::Parse("Ctrl+F5")->vk == 0x74);
    EXPECT_TRUE(!iroha::KeyShortcut::Parse("Enter"));     // 修飾キーが無い
    EXPECT_TRUE(!iroha::KeyShortcut::Parse("Cmd+Enter")); // Windows に無い
    EXPECT_TRUE(!iroha::KeyShortcut::Parse(""));
    const auto decoded = iroha::KeyShortcut::Decode(iroha::KeyShortcut::Parse("Ctrl+Alt+Enter")->Encode());
    EXPECT_TRUE(decoded.vk == 0x0D && decoded.control && decoded.alt && !decoded.shift);
}

void RunAITests() {
    TestAIRequestBuild();
    TestAIResponseParsing();
    TestKeyShortcut();
}

} // namespace
