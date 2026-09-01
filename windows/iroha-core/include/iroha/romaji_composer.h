#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace iroha {

// ローマ字入力を逐次ひらがなに変換する合成器。
// 移植元: macos/Sources/IrohaCore/RomajiComposer.swift（挙動互換を維持すること）
// MS-IME/mozc互換の規則:
//   - 「nn」「n'」→「ん」、「n」+子音 →「ん」+子音
//   - 同一子音の連続 →「っ」（tch も「っ」+ ch）
//   - 対応しない文字はそのまま通す
class RomajiComposer {
public:
    // comma/period: 「,」「.」キーで入力する文字（既定は「、」「。」）
    explicit RomajiComposer(std::u32string comma = U"、", std::u32string period = U"。");

    void Input(char32_t character);
    void Input(const std::u32string& text);

    // 表示上の末尾1文字を削除する
    void DeleteBackward();

    // 未解決の末尾を確定する（"n" → "ん"、それ以外はそのまま残す）
    void Flush();

    void Clear();

    // 画面表示用の未確定文字列（かな + 未解決ローマ字）
    std::u32string Display() const { return text_ + pending_; }
    bool Empty() const { return text_.empty() && pending_.empty(); }

    // 確定済みのかな文字列
    const std::u32string& Text() const { return text_; }
    // 未解決のローマ字（例: "ky" の状態）
    const std::u32string& Pending() const { return pending_; }
    // 打鍵されたままの文字列（F9/F10の英数変換用）
    const std::u32string& Raw() const { return raw_; }
    // rawが表示内容と対応しているか（かな部分を削除するとずれるためfalseになる）
    bool RawIsReliable() const { return rawIsReliable_; }

private:
    void Resolve();
    void ForceResolveHead();

    std::u32string text_;
    std::u32string pending_;
    std::u32string raw_;
    bool rawIsReliable_ = true;

    // 変換テーブル（句読点スタイルを反映したもの）
    std::unordered_map<std::u32string, std::u32string> table_;
};

} // namespace iroha
