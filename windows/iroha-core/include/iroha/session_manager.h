#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "iroha/input_session.h"

namespace iroha {

// 入力セッション（InputSession）の表と、ライブ変換を裏で行うスレッド。変換サーバが 1 つ持つ。
//
// キーの処理（HandleKey）はすぐ戻る: ライブ変換は裏のスレッドに頼み、届いたらセッションに渡す。
// TIP は Poll で表示が変わったかを確かめる。Space での変換や候補の取得はキーの処理の中で待つ。
//
// 変換エンジンは 1 つで、同時に 1 つの変換しか行えない。キーの処理が変換を必要としたときは、
// 進行中のライブ変換をやめさせて先に行う（そのライブ変換の結果は、たいていもう要らない）。
// 新しいライブ変換の依頼では、進行中のものをやめさせない（結果を頭の部分の変換として使う。
// InputSession::CompleteLiveConversion 参照）。始めていない古い依頼は捨てる。
//
// HandleKey / Poll / End / WithEngine / SetLiveConversionEnabled は 1 つのスレッドから呼ぶこと
// （変換サーバのパイプのスレッド）。裏のスレッドとの間はこのクラスが直列化する
class SessionManager {
public:
    // 変換エンジンと学習の呼び出し口。どれも、エンジンをほかが使っていない状態で呼ぶ
    class Backend {
    public:
        virtual ~Backend() = default;
        virtual bool Convert(const std::u32string& reading, const std::u32string& context,
                             int candidateCount, std::vector<std::u32string>* results,
                             std::string* error) = 0;
        // 変換を途中でやめるかの問い合わせ口を渡す（空の関数で外す）。
        // 渡されている間、Convert は区切りのよいところで呼んで確かめ、真なら失敗として戻る
        virtual void SetCancelCheck(std::function<bool()> check) = 0;
        // 文節変換で確定した（学習に使う。引数は InputSession::Host::RecordCommit と同じ）
        virtual void RecordCommit(const std::u32string& reading, const std::u32string& engineBaseline,
                                  const std::u32string& baseline, const std::u32string& committed) = 0;
        // 学習が差し替える前のエンジンの結果（InputSession::Host::EngineResult）
        virtual std::optional<std::u32string> EngineResult(const std::u32string& reading,
                                                           const std::u32string& shown) {
            (void)reading;
            (void)shown;
            return std::nullopt;
        }
        // 学習を消す（InputSession::Host::ForgetLearning）
        virtual void ForgetLearning(const std::u32string& reading, const std::u32string& engineResult,
                                    const std::u32string& shown) {
            (void)reading;
            (void)engineResult;
            (void)shown;
        }
        // 句読点の組を切り替えた（設定に保存する）。エンジンを持たずに呼ぶ
        virtual void PunctuationChanged(const std::u32string& punctuation) { (void)punctuation; }
        // ユーザ辞書の候補ウィンドウ専用の語（InputSession::Host::CandidateOnlyWords）
        virtual std::vector<std::u32string> CandidateOnlyWords(const std::u32string& reading) {
            (void)reading;
            return {};
        }
        // 予測変換の候補・入力履歴の記録（InputSession::Host と同じ。エンジンを持たずに呼ぶ）
        virtual std::vector<InputHistoryEntry> PredictionCandidates(const std::u32string& prefix) {
            (void)prefix;
            return {};
        }
        virtual void RecordInputHistory(const std::u32string& reading, const std::u32string& text) {
            (void)reading;
            (void)text;
        }
        // プライベートモードを切り替えた（設定に保存し、変換エンジンの学習・ユーザ辞書を止める）。エンジンを持たずに呼ぶ
        virtual void PrivateModeChanged(bool enabled) { (void)enabled; }
        virtual void RecordConversion(const ConversionLogEntry& entry) { (void)entry; }
        // 打ち間違いの訂正（InputSession::Host と同じ。エンジンを持たずに呼ぶ。訂正のモデルは自分で直列化すること）
        virtual bool TypoCorrectionAvailable() { return false; }
        // AIで直す: モデル単体で貪欲に変換し、文字ごとのマージンを返す（エンジンを持った状態で呼ぶ）。
        // 求められなければ text が空
        struct ScoredText {
            std::u32string text;
            std::vector<float> margins;
        };
        virtual ScoredText ScoreConversion(const std::u32string& reading, const std::u32string& context) {
            (void)reading;
            (void)context;
            return {};
        }
        // AI に依頼する（AIで直す）。stallSeconds の間何も届かなければ打ち切る。エンジンを持たずに呼ぶ
        virtual AIOutcome RunAIRequest(const AIRequest& request, int stallSeconds, CancelToken* cancel) {
            (void)request;
            (void)stallSeconds;
            (void)cancel;
            return {};
        }
        // AI変換（preset 番目のプリセットで text を）。途中経過を onPartial に渡し、結果（失敗なら無し）を返す。
        // cancel が止められたら途中でも戻る。エンジンを持たずに呼ぶ
        virtual std::optional<std::u32string> RunAI(size_t preset, const std::u32string& text,
                                                    const std::function<void(const std::u32string&)>& onPartial,
                                                    CancelToken* cancel) {
            (void)preset;
            (void)text;
            (void)onPartial;
            (void)cancel;
            return std::nullopt;
        }
        virtual std::optional<TypoCorrection> CorrectTypo(const std::u32string& reading, double threshold,
                                                          size_t minimumLength) {
            (void)reading;
            (void)threshold;
            (void)minimumLength;
            return std::nullopt;
        }
        // 「こえ」モード: 録音と認識を始める（すぐ戻ること。同時に 1 つだけで、行っている回があればそれは捨てる）。
        // onEvent は裏のスレッドから呼ぶ。1 回につき、終わりの知らせ（Finished・Failed・Discarded）を必ず 1 回渡す。
        // 始められなければ理由を返す（空なら始めた。onEvent は呼ばない）。
        // SessionManager を壊す前に、onEvent の呼び出しが終わっていること
        virtual std::u32string StartVoice(std::function<void(const InputSession::VoiceEvent&)> onEvent) {
            (void)onEvent;
            return U"音声認識を使えません";
        }
        // 録音をやめて、そこまでの音声の認識を終える
        virtual void StopVoice() {}
        // 録音と認識をやめる（終わりの知らせは Discarded）
        virtual void CancelVoice() {}
    };

    explicit SessionManager(Backend* backend);
    ~SessionManager();
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // キー 1 つを処理する。知らない sessionId なら新しいセッションを作る
    SessionOutput HandleKey(uint64_t sessionId, SessionKey key, char32_t character, bool shift,
                            const std::u32string* leftContext);
    // revision のときから表示が変わっていれば、真を返して output に今の表示を入れる
    bool Poll(uint64_t sessionId, uint64_t revision, SessionOutput* output);
    // セッションを捨てる（TIP が無効になった）
    void End(uint64_t sessionId);

    // エンジンを独占して work を行う（進行中のライブ変換はやめさせる）。
    // 設定の読み直しと、セッションを通さない変換に使う
    void WithEngine(const std::function<void()>& work);

    // セッションの設定（今あるセッションと、これから作るセッションに効く）
    void SetSettings(const SessionSettings& settings);
    // ライブ変換を使うか（SetSettings の一部だけを変える）
    void SetLiveConversionEnabled(bool enabled);
    // 文節の候補に合流させる記号・顔文字・絵文字の表（nullptr なら合流させない。同じく全セッションに効く）
    void SetSymbolTable(std::shared_ptr<const SymbolCandidateTable> table);
    // 候補に専門辞書の名前の印を付けるための表（nullptr なら付けない。同じく全セッションに効く）
    void SetTermLookup(std::shared_ptr<const TermDictionaryLookup> lookup);
    // 候補に合流させる変換ルール（nullptr なら合流させない。同じく全セッションに効く）
    void SetRewriteRules(std::shared_ptr<const UserRewriteRuleSet> rules);

    size_t SessionCount();
    // どれかのセッションが入力中（未確定文字列がある）か。選択テキストのAI編集のショートカットを、入力中は無視するのに使う
    bool AnyComposing();

    // 持っておくセッションの数。超えたら、最後に使ったのがもっとも古いものから捨てる
    // （TIP が終了を知らせずに消えた分が溜まらないように）
    static constexpr size_t kMaxSessions = 64;

private:
    struct Entry;
    struct Job {
        uint64_t serial = 0;
        uint64_t sessionId = 0;
        uint64_t generation = 0;
        std::u32string reading;
        std::u32string context;
    };
    class EngineLock;

    Entry& EntryFor(uint64_t sessionId);
    void Enqueue(uint64_t sessionId, uint64_t generation, const std::u32string& reading,
                 const std::u32string& context);
    void CancelJobs(uint64_t sessionId);
    void WorkerLoop();
    // 入力の休止を待って行う依頼（打ち間違いの訂正・AIで直すの判定。セッションと種類ごとに最新の 1 件。
    // 休止の時間が来たら裏のスレッドで行う）
    struct TypoJob {
        enum class Kind { Typo, AIFixCheck };
        Kind kind = Kind::Typo;
        uint64_t generation = 0;
        std::u32string reading;
        std::u32string context; // AIFixCheck: 変換の左文脈
        double threshold = 0;
        size_t minimumLength = 1;
        std::chrono::steady_clock::time_point due;
    };
    void EnqueueTypo(uint64_t sessionId, TypoJob job);
    // AI変換の依頼（セッションごとに最新の 1 件。裏のスレッドで 1 件ずつ行う）
    struct AIJob {
        uint64_t sessionId = 0;
        uint64_t generation = 0;
        size_t preset = 0;
        std::u32string text;
        // AIで直すの依頼なら、その中身（AI変換なら無し）
        std::optional<AIRequest> fix;
    };
    void EnqueueAI(AIJob job);
    void CancelAIJobs(uint64_t sessionId);
    void AIWorkerLoop();
    void CancelTypo(uint64_t sessionId, TypoJob::Kind kind = TypoJob::Kind::Typo);
    void TypoWorkerLoop();

    Backend* backend_;
    // 「こえ」モードで録音・認識しているセッション（0 なら無し。stateMutex_ で守る）
    uint64_t voiceSessionId_ = 0;

    // セッションの表（stateMutex_ で守る）
    std::mutex stateMutex_;
    std::map<uint64_t, std::unique_ptr<Entry>> sessions_;
    uint64_t useCounter_ = 0;
    SessionSettings settings_;
    std::shared_ptr<const SymbolCandidateTable> symbolTable_;
    std::shared_ptr<const TermDictionaryLookup> termLookup_;
    std::shared_ptr<const UserRewriteRuleSet> rewriteRules_;

    // エンジン。キーの処理が待っている間（engineWaiters_ > 0）、ライブ変換は途中でやめる。
    // 鍵を取る順は stateMutex_ → engineMutex_（裏のスレッドは engineMutex_ を離してから stateMutex_）
    std::mutex engineMutex_;
    std::atomic<int> engineWaiters_{0};

    // ライブ変換の依頼（jobMutex_ で守る）。待たせておくのは最新の 1 件だけ
    std::mutex jobMutex_;
    std::condition_variable jobCondition_;
    std::optional<Job> pendingJob_;
    uint64_t jobSerial_ = 0;
    uint64_t runningSerial_ = 0; // 0 なら実行中のものは無い
    uint64_t runningSessionId_ = 0;
    bool stopping_ = false;
    // この番号までの依頼は、実行中でも途中でやめる
    std::atomic<uint64_t> abortedSerial_{0};
    std::atomic<bool> stoppingFlag_{false};

    std::thread worker_;

    // 打ち間違いの訂正（typoMutex_ で守る）。ライブ変換とは別のスレッドで行う（変換エンジンを使わない）
    std::mutex typoMutex_;
    std::condition_variable typoCondition_;
    std::map<std::pair<uint64_t, TypoJob::Kind>, TypoJob> typoJobs_;
    bool typoStopping_ = false;
    std::thread typoWorker_;

    // AI変換（aiMutex_ で守る）。行っている依頼は止められるようにしておく
    std::mutex aiMutex_;
    std::condition_variable aiCondition_;
    std::vector<AIJob> aiJobs_;
    std::optional<AIJob> aiRunning_;
    std::shared_ptr<CancelToken> aiCancel_;
    bool aiStopping_ = false;
    std::thread aiWorker_;
};

} // namespace iroha
