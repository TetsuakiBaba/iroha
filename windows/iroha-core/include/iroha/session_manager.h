#pragma once
#include <atomic>
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
        // 文節変換で確定した（学習に使う）
        virtual void RecordCommit(const std::u32string& reading, const std::u32string& baseline,
                                  const std::u32string& committed) = 0;
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

    // ライブ変換を使うか（今あるセッションと、これから作るセッションに効く）
    void SetLiveConversionEnabled(bool enabled);
    // 文節の候補に合流させる記号・顔文字・絵文字の表（nullptr なら合流させない。同じく全セッションに効く）
    void SetSymbolTable(std::shared_ptr<const SymbolCandidateTable> table);
    // 候補に専門辞書の名前の印を付けるための表（nullptr なら付けない。同じく全セッションに効く）
    void SetTermLookup(std::shared_ptr<const TermDictionaryLookup> lookup);

    size_t SessionCount();

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

    Backend* backend_;

    // セッションの表（stateMutex_ で守る）
    std::mutex stateMutex_;
    std::map<uint64_t, std::unique_ptr<Entry>> sessions_;
    uint64_t useCounter_ = 0;
    bool liveConversionEnabled_ = true;
    std::shared_ptr<const SymbolCandidateTable> symbolTable_;
    std::shared_ptr<const TermDictionaryLookup> termLookup_;

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
};

} // namespace iroha
