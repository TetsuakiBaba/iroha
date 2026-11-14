#pragma once
// 1 回の変換要求（または打ち間違いの訂正 1 回）の中で、ニューラルネット（NN）の推論にかかった時間を集める
// （デバッグ表示に出す。移植元: macos/Sources/IrohaCore/InferenceTimer.swift）。
//
// 変換エンジンはデコレータを重ねた形なので、呼び出し側で測った時間には NN 以外の処理が混ざる。NN の時間は
// NN を動かす側（ZenzEngine・TypoNormalizer）の内側でしか測れないため、呼び出し側がスレッドごとの入れ物を
// 置き（Scope）、NN を動かす側が計算の前後で時刻を取って足し込む（Measure）。macOS版のタスクローカル値の代わりに
// スレッドローカルにしている（変換は呼び出したスレッドの中で終わる）。入れ物が無ければ時刻も取らない
#include <chrono>
#include <cstdint>

namespace iroha {

struct InferenceStats {
    // NN を通さずに結果を返した理由（NN の回数が 0 のときの説明に使う）
    enum Shortcut : unsigned {
        kLearning = 1u << 0,        // 学習（エンジンが失敗し、読み全体が一致した学習の結果だけで返した）
        kUserDictionary = 1u << 1,  // ユーザ辞書（読み全体が 1 語と一致）
        kCache = 1u << 2,           // 長い読みの区切りのキャッシュ
    };
    // NN の計算にかかった時間の合計（モデルの読み込みは含まない）と回数（辞書ラティスでは生成と採点で 2 回）
    int64_t neuralNetworkMicros = 0;
    int neuralNetworkCalls = 0;
    // モデルの読み込みにかかった時間（最初の 1 回だけ 0 でない）
    int64_t modelLoadMicros = 0;
    unsigned shortcuts = 0;
};

// 1 回の要求を測った結果（全体の時間と、その中の NN の内訳）
struct InferenceReport {
    int64_t totalMicros = 0;
    InferenceStats stats;
};

namespace inference_timer {

// このスレッドに置かれた入れ物（無ければ nullptr）
InferenceStats* Current();

// 入れ物をこのスレッドに置く（抜けると前のものに戻す）
class Scope {
public:
    explicit Scope(InferenceStats* stats);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    InferenceStats* previous_;
};

// 生きている間の時間を NN（回数も 1 足す）かモデルの読み込みとして足す。入れ物が無ければ何もしない
class Measure {
public:
    enum class Kind { NeuralNetwork, ModelLoad };
    explicit Measure(Kind kind);
    ~Measure();
    Measure(const Measure&) = delete;
    Measure& operator=(const Measure&) = delete;

private:
    Kind kind_;
    InferenceStats* stats_;
    std::chrono::steady_clock::time_point start_;
};

// NN を通さずに返した理由を記す
void Note(InferenceStats::Shortcut shortcut);

} // namespace inference_timer
} // namespace iroha
