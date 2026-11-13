#pragma once
#include <optional>
#include <string>
#include <vector>

#include "iroha/typo_normalizer.h"

namespace iroha {

// 「AIで直す」（誤変換の疑いがあるときに AI に直させる。既定 OFF）の規則。移植元: macos/Sources/IrohaCore/AIFixRule.swift。
//
// 疑いの判定は、モデルが変換結果を生成したときの文字ごとのマージン（読み制約を満たす 1 位と 2 位の
// トークンの対数確率の差）の最小値で行う。判定するのは、表示しているライブ変換の結果がモデルの貪欲生成
// そのままのときだけ（学習・ユーザ辞書で差し替わった表示はユーザが選んだ結果なので疑わない）。
// AI への依頼の文面と答えの読み方は macOS版（測定で使ったもの）と同じにしてある。変えたら測り直す
namespace AIFixRule {

// これより長い読みは判定しない（ライブ変換は長い読みを区切って変換するので、1 回の生成の結果と表示が対応しない）
constexpr size_t kMaximumReadingLength = 80;
// 閾値の既定（nat）
constexpr double kDefaultThreshold = 1.0;

// 文字ごとのマージンの最小値。競合するトークンが無かった文字（+inf）しかなければ +inf
float MinimumMargin(const std::vector<float>& margins);

// 誤変換の疑いがあるか。scoredText・margins はモデル単体で同じ読み・文脈を貪欲に変換した結果、shown は表示中の変換
bool IsSuspicious(const std::u32string& scoredText, const std::vector<float>& margins, const std::u32string& shown,
                  const std::u32string& reading, double threshold);

// AI への指示（macOS版と同じ文面）
extern const char32_t kInstructions[];

// AI に渡す本文。読みはカタカナにして渡す
std::u32string Message(const std::u32string& context, const std::u32string& reading, const std::u32string& conversion);

// 「今の変換 → AI の案」を出すための割り付け（打ち間違いの訂正の知らせと同じ形・同じ切り詰め方）。同じなら無し
std::optional<TypoCorrectionFeedback> Feedback(const std::u32string& original, const std::u32string& corrected,
                                               size_t context = 6, size_t maxChanged = 12);

// AI の答えから直した変換結果を取り出す。複数行なら最後の空でない行。変換結果の 2 倍を超える長さは捨てる
std::optional<std::u32string> Answer(const std::u32string& output, const std::u32string& current);

} // namespace AIFixRule

} // namespace iroha
