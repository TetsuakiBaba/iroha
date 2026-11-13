#pragma once
// 「こえ」モードの録音と認識（変換サーバ・iroha-cli で使う）。
//
// 認識は SAPI（Windows の音声認識。端末内で動く）の日本語の認識エンジンで行う（ユーザの判断。2026-11-14）。
// マイクの音声を waveIn で受け、書き足されるまで読み取りを待たせるストリームで SAPI に流す。
// 話している間は途中結果（仮説）が届き、キーを離したらストリームを閉じて、そこまでの音声の結果を待つ。
// 結果は語ごとの表示形・語彙形から読みを付けて渡す（iroha::VoiceTranscript::TokenFromRecognition）。
// 移植元の macOS版は SpeechAnalyzer（macos/Sources/IrohaVoice/）で、録音・認識の仕組みは別物
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "iroha/input_session.h"

namespace iroha {

class VoiceRecognizer {
public:
    using EventHandler = std::function<void(const InputSession::VoiceEvent&)>;

    VoiceRecognizer();
    // 行っている回を止め、裏のスレッドが終わるのを待つ（知らせを受け取る側より先に壊すこと）
    ~VoiceRecognizer();
    VoiceRecognizer(const VoiceRecognizer&) = delete;
    VoiceRecognizer& operator=(const VoiceRecognizer&) = delete;

    // 日本語の認識エンジンがあるか（name にエンジンの名前）
    static bool Available(std::u32string* name = nullptr);

    // マイクから録音して認識する。すぐ戻り、知らせは裏のスレッドから handler に渡す
    // （Started・Partial のあと、終わりの知らせ Finished・Failed・Discarded のどれかを必ず 1 回）。
    // 押してから kStartDelayMs 待ってマイクを開く（その前に Stop・Cancel されたら Discarded）。
    // 行っている回があれば、それは捨てる。始められなければ理由を返す（空なら始めた）
    std::u32string Start(EventHandler handler);
    // 録音をやめ、そこまでの音声の結果を待つ。押してから kMinimumHoldMs に満たなければ捨てる（Discarded）
    void Stop();
    // 録音と認識をやめる（Discarded）
    void Cancel();
    // 行っている回が終わるのを待つ（iroha-cli 用）
    void Wait();
    // マイクの代わりに音声ファイル（WAV）を実時間の速さで流す（確かめる用。変換サーバの IROHA_VOICE_FILE）。
    // キーの扱い（待ち・短すぎたら捨てる・離したら閉じる）はマイクと同じ。空ならマイク
    void SetFileSource(const std::wstring& path) { fileSource_ = path; }

    // 音声ファイル（WAV）を認識する（iroha-cli 用。戻るまで待つ）。realtime なら実時間の速さで流す
    // （マイクと同じく、途中結果が届く時刻を確かめられる）
    static bool RecognizeFile(const std::wstring& path, bool realtime, const EventHandler& handler, std::string* error);

    // 押してからマイクを開くまでの待ち（ショートカットに使うたびにマイクが動かないように。macOS版 startDelay）
    static constexpr int kStartDelayMs = 120;
    // 押していた時間がこれより短ければ捨てる（macOS版 minimumHold）
    static constexpr int kMinimumHoldMs = 350;
    // 押し続けていてもここで締める（macOS版 maximumHold）
    static constexpr int kMaximumHoldMs = 60000;
    // 離してから結果を待つ上限
    static constexpr int kFinishTimeoutMs = 15000;

    struct Utterance;
    struct Shared;

private:
    std::wstring fileSource_;
    std::shared_ptr<Shared> shared_;
    std::mutex mutex_;
    std::shared_ptr<Utterance> current_;
};

} // namespace iroha
