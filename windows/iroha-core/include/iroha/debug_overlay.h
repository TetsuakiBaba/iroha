#pragma once
// デバッグ表示（開発者モードの「推論にかかった時間と左文脈をカーソルの右下に表示する」）の行を作る。
// 移植元: macos/Sources/iroha/DeveloperOverlay.swift の行の組み立て（文面も同じ）。
// 行は「左文脈」「かな漢字変換」「打ち間違いの訂正」の 3 本（macOS版の「要素の文字」はアクセシビリティで
// 要素の文字数を調べる macOS 専用の行なので無い。加速度センサのグラフも無い）。
// 窓に出すのはフロントエンド（IBus・fcitx5）で、ここは文字列だけを作る
#include <cstdint>
#include <optional>
#include <string>

#include "iroha/inference_timer.h"

namespace iroha::debug_overlay {

// 変換要求の種類（辞書ラティスを通すか・何回変換するかが違うので、行の頭に出す）
enum class ConversionKind { Live, Segmenting, Resegmenting, Candidates };
// 打ち間違いの訂正を走らせたきっかけ
enum class TypoTrigger { Idle, Conversion };
// 訂正の結果
enum class TypoOutcome { Corrected, None, TrailingInsertion };
// 入力を始めたときの左文脈の出どころ
enum class ContextSource {
    Document,    // アプリから読めた（周辺テキスト）
    Disabled,    // アプリから読む設定が OFF
    Unavailable, // アプリが周辺テキストを返さない
};

// 「左文脈  アプリから 12文字「…」」。アプリから読めなかったときの fallback は代わりに使う確定文字列の蓄積
std::u32string ContextLine(ContextSource source, const std::u32string& documentText,
                           const std::optional<std::u32string>& fallback);
// かな漢字変換 1 回ぶん（cancelled はその前に結果を使わずに打ち切った変換の数）
std::u32string ConversionLine(ConversionKind kind, int64_t totalMicros, const InferenceStats& stats,
                              size_t readingLength, int cancelled);
// 打ち間違いの訂正 1 回ぶん
std::u32string TypoLine(TypoTrigger trigger, int64_t totalMicros, const InferenceStats& stats, size_t readingLength,
                        TypoOutcome outcome);
// 100ms 未満は小数 1 桁、それ以上は整数の ms
std::u32string FormatDuration(int64_t micros);

} // namespace iroha::debug_overlay
