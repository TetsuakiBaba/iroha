#pragma once
// セッションの表とライブ変換のスレッド（SessionManager）のテスト。Windows 版で足したもの。
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "iroha/kana.h"
#include "iroha/session_manager.h"

namespace {

// 変換の呼び出しを記録する偽物。ライブ変換（途中でやめる問い合わせ口が渡されている変換）は、
// holdLive が真の間、やめるよう言われるまで終わらない
class FakeSessionBackend : public iroha::SessionManager::Backend {
public:
    bool Convert(const std::u32string& reading, const std::u32string& context, int candidateCount,
                 std::vector<std::u32string>* results, std::string* error) override {
        const bool live = static_cast<bool>(cancelCheck_);
        {
            std::lock_guard<std::mutex> lock(mutex);
            calls.push_back({reading, context, candidateCount, live});
        }
        if (live) {
            ++liveStarted;
            while (holdLive.load()) {
                if (cancelCheck_()) {
                    ++liveCancelled;
                    if (error) *error = "取り消し";
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        results->clear();
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = table.find(reading);
        results->push_back(it != table.end() ? it->second : iroha::HiraganaToKatakana(reading));
        return true;
    }

    void SetCancelCheck(std::function<bool()> check) override { cancelCheck_ = std::move(check); }

    void RecordCommit(const std::u32string& reading, const std::u32string& baseline,
                      const std::u32string& committed) override {
        std::lock_guard<std::mutex> lock(mutex);
        commits.push_back(reading + U"/" + baseline + U"/" + committed);
    }

    struct Call {
        std::u32string reading;
        std::u32string context;
        int count;
        bool live;
    };

    std::vector<Call> Calls() {
        std::lock_guard<std::mutex> lock(mutex);
        return calls;
    }

    std::mutex mutex;
    std::map<std::u32string, std::u32string> table;
    std::vector<Call> calls;
    std::vector<std::u32string> commits;
    std::atomic<bool> holdLive{false};
    std::atomic<int> liveStarted{0};
    std::atomic<int> liveCancelled{0};

private:
    // エンジンを持っているスレッドだけが触る
    std::function<bool()> cancelCheck_;
};

bool WaitUntil(const std::function<bool()>& condition, int timeoutMs = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

iroha::SessionOutput TypeKeys(iroha::SessionManager& manager, uint64_t sessionId,
                              const std::u32string& keys) {
    iroha::SessionOutput output;
    for (char32_t c : keys) {
        output = manager.HandleKey(sessionId, iroha::SessionKey::Character, c, false, nullptr);
    }
    return output;
}

// ライブ変換が終わるまで Poll し、最後の表示を返す
iroha::SessionOutput PollUntilSettled(iroha::SessionManager& manager, uint64_t sessionId,
                                      iroha::SessionOutput output) {
    WaitUntil([&] {
        iroha::SessionOutput next;
        if (manager.Poll(sessionId, output.revision, &next)) output = next;
        return !output.livePending;
    });
    return output;
}

// キーの処理はすぐ戻り、ライブ変換の結果は Poll で届く
void TestManagerLiveConversionArrivesByPoll() {
    FakeSessionBackend backend;
    backend.table[U"きょう"] = U"今日";
    {
        iroha::SessionManager manager(&backend);
        auto output = TypeKeys(manager, 1, U"kyou");
        EXPECT_TRUE(output.Composing());
        output = PollUntilSettled(manager, 1, output);
        EXPECT_EQ(output.PreeditText(), U"今日");
        EXPECT_TRUE(!output.livePending);

        // 変わっていなければ Poll は偽。知らないセッションも偽
        iroha::SessionOutput unused;
        EXPECT_TRUE(!manager.Poll(1, output.revision, &unused));
        EXPECT_TRUE(!manager.Poll(999, 0, &unused));

        output = manager.HandleKey(1, iroha::SessionKey::Enter, 0, false, nullptr);
        EXPECT_EQ(output.commit, U"今日");
    }
}

// 始めたライブ変換は読みが伸びても最後まで行い、頭の部分の変換として使う。
// 始めていない古い依頼は捨てて、最新の読みを変換する
void TestManagerKeepsRunningConversionAndDropsQueued() {
    FakeSessionBackend backend;
    backend.table[U"きょ"] = U"巨";
    backend.table[U"きょうは"] = U"今日は";
    backend.holdLive = true;
    {
        iroha::SessionManager manager(&backend);
        TypeKeys(manager, 1, U"kyo");
        EXPECT_TRUE(WaitUntil([&] { return backend.liveStarted.load() == 1; }));
        auto output = TypeKeys(manager, 1, U"uha"); // きょう → きょうは（きょう は始まる前に置き換わる）
        EXPECT_EQ(output.PreeditText(), U"きょうは");
        backend.holdLive = false;
        output = PollUntilSettled(manager, 1, output);
        EXPECT_EQ(output.PreeditText(), U"今日は");
        EXPECT_TRUE(backend.liveCancelled.load() == 0);
    }
    const auto calls = backend.Calls();
    EXPECT_TRUE(calls.size() == 2);
    if (calls.size() == 2) {
        EXPECT_EQ(calls[0].reading, U"きょ");
        EXPECT_EQ(calls[1].reading, U"きょうは");
    }
}

// Space の変換は、進行中のライブ変換をやめさせて先に行う
void TestManagerKeyConversionPreemptsLive() {
    FakeSessionBackend backend;
    backend.table[U"きしゃ"] = U"記者";
    backend.holdLive = true;
    {
        iroha::SessionManager manager(&backend);
        TypeKeys(manager, 1, U"kisha");
        EXPECT_TRUE(WaitUntil([&] { return backend.liveStarted.load() >= 1; }));
        auto output = manager.HandleKey(1, iroha::SessionKey::Space, 0, false, nullptr);
        EXPECT_TRUE(output.segmenting && !output.livePending);
        EXPECT_EQ(output.PreeditText(), U"記者");
        EXPECT_TRUE(backend.liveCancelled.load() >= 1);

        output = manager.HandleKey(1, iroha::SessionKey::Enter, 0, false, nullptr);
        EXPECT_EQ(output.commit, U"記者");
        // やめさせた変換の結果があとから表示に混ざらない
        iroha::SessionOutput later;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (manager.Poll(1, output.revision, &later)) EXPECT_TRUE(!later.Composing());
    }
}

// 文節変換で確定すると学習に知らせる。WithEngine はライブ変換をやめさせて行う
void TestManagerRecordsCommitAndRunsExclusiveWork() {
    FakeSessionBackend backend;
    backend.table[U"きしゃ"] = U"記者";
    {
        iroha::SessionManager manager(&backend);
        TypeKeys(manager, 1, U"kisha");
        manager.HandleKey(1, iroha::SessionKey::Space, 0, false, nullptr);
        manager.HandleKey(1, iroha::SessionKey::Enter, 0, false, nullptr);
        {
            std::lock_guard<std::mutex> lock(backend.mutex);
            EXPECT_TRUE(backend.commits.size() == 1);
            if (backend.commits.size() == 1) EXPECT_EQ(backend.commits[0], U"きしゃ/記者/記者");
        }

        backend.holdLive = true;
        const int startedBefore = backend.liveStarted.load();
        TypeKeys(manager, 1, U"ka");
        EXPECT_TRUE(WaitUntil([&] { return backend.liveStarted.load() > startedBefore; }));
        bool ran = false;
        manager.WithEngine([&] { ran = true; });
        EXPECT_TRUE(ran);
        backend.holdLive = false;
    }
}

// ライブ変換を使わない設定では変換を頼まない。セッションは終了の知らせと上限で捨てる
void TestManagerSessionsAndSettings() {
    FakeSessionBackend backend;
    {
        iroha::SessionManager manager(&backend);
        manager.SetLiveConversionEnabled(false);
        auto output = TypeKeys(manager, 1, U"kyou");
        EXPECT_TRUE(!output.livePending);
        EXPECT_EQ(output.PreeditText(), U"きょう");
        EXPECT_TRUE(manager.SessionCount() == 1);
        manager.End(1);
        EXPECT_TRUE(manager.SessionCount() == 0);

        for (uint64_t id = 1; id <= iroha::SessionManager::kMaxSessions + 5; ++id) {
            TypeKeys(manager, id, U"a");
        }
        EXPECT_TRUE(manager.SessionCount() == iroha::SessionManager::kMaxSessions);
        // 古いものから捨てたので、最初のセッションは空から始まる
        output = TypeKeys(manager, 1, U"i");
        EXPECT_EQ(output.PreeditText(), U"い");
    }
    EXPECT_TRUE(backend.Calls().empty());
}

void RunSessionManagerTests() {
    TestManagerLiveConversionArrivesByPoll();
    TestManagerKeepsRunningConversionAndDropsQueued();
    TestManagerKeyConversionPreemptsLive();
    TestManagerRecordsCommitAndRunsExclusiveWork();
    TestManagerSessionsAndSettings();
}

} // namespace
