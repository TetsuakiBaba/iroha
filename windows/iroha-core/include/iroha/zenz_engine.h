#pragma once
#include <memory>
#include <string>
#include <vector>

#include "iroha/conversion_engine.h"

namespace iroha {

// zenz-v3（GPT-2系かな漢字変換モデル）をllama.cppで動かす変換エンジン。
// 移植元: macos/Sources/IrohaCore/ZenzEngine.swift（挙動互換を維持すること）
//
// プロンプト形式（zenz-v3）:
//   [U+EE02 + 左文脈] + U+EE00 + カタカナ読み + U+EE01 → 変換結果
//
// スレッドセーフではない。呼び出し側（変換サーバ）で直列化すること。
// このクラスはllama.cppに依存するため iroha-engine ターゲットに属する
// （TIP DLLにはリンクしないこと）。
class ZenzEngine : public ConversionEngine {
public:
    // modelPath: GGUFファイルへのパス（UTF-8）
    explicit ZenzEngine(std::string modelPath);
    ~ZenzEngine() override;
    ZenzEngine(const ZenzEngine&) = delete;
    ZenzEngine& operator=(const ZenzEngine&) = delete;

    // モデルを事前にロードしておく（初回変換のもたつき防止）
    bool Prewarm(std::string* error);

    // ひらがな読みを仮名漢字混じり文候補（尤度順）に変換する。
    // candidateCountが1ならグリーディ、2以上で先頭トークン分岐のn-best
    bool Convert(const std::u32string& reading, const std::u32string& leftContext,
                 int candidateCount, std::vector<std::u32string>* results,
                 std::string* error) override;

    static std::u32string BuildPrompt(const std::u32string& reading,
                                      const std::u32string& leftContext,
                                      size_t maxContextLength);

private:
    struct Runtime;

    bool EnsureLoaded(std::string* error);

    // 左文脈として与える最大文字数（zenz-v3の学習設定に合わせる）
    static constexpr size_t kMaxContextLength = 40;

    std::string modelPath_;
    std::unique_ptr<Runtime> runtime_;
};

} // namespace iroha
