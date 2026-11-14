#pragma once
// 入力セッション（InputSession）のテスト。Windows 版で足したもの
// （キーの扱い・表示と確定の規則は macOS版 IrohaInputController と同じにしてある）。
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "iroha/input_session.h"
#include "iroha/kana.h"

namespace {

// 変換と学習の呼び出しを記録する偽物。ライブ変換はテストが手で結果を渡す
class FakeSessionHost : public iroha::InputSession::Host {
public:
    struct LiveRequest {
        uint64_t generation;
        std::u32string reading;
        std::u32string context;
    };
    struct ConvertCall {
        std::u32string reading;
        std::u32string context;
        int count;
    };
    struct Commit {
        std::u32string reading;
        std::u32string engineBaseline;
        std::u32string baseline;
        std::u32string committed;
    };

    bool Convert(const std::u32string& reading, const std::u32string& context, int candidateCount,
                 std::vector<std::u32string>* results, std::string* error) override {
        convertCalls.push_back({reading, context, candidateCount});
        if (shouldFail) {
            if (error) *error = "テスト";
            return false;
        }
        results->clear();
        if (candidateCount <= 1) {
            const auto it = table.find(reading);
            results->push_back(it != table.end() ? it->second : iroha::HiraganaToKatakana(reading));
            return true;
        }
        const auto it = candidateTable.find(reading);
        if (it != candidateTable.end()) {
            *results = it->second;
        } else {
            results->push_back(iroha::HiraganaToKatakana(reading));
        }
        return true;
    }

    void RequestLiveConversion(uint64_t generation, const std::u32string& reading,
                               const std::u32string& context) override {
        liveRequests.push_back({generation, reading, context});
    }

    void CancelLiveConversion() override { ++liveCancels; }

    void RecordCommit(const std::u32string& reading, const std::u32string& engineBaseline,
                      const std::u32string& baseline, const std::u32string& committed) override {
        commits.push_back({reading, engineBaseline, baseline, committed});
    }

    std::optional<std::u32string> EngineResult(const std::u32string& reading,
                                               const std::u32string& shown) override {
        const auto it = engineResults.find({reading, shown});
        if (it == engineResults.end()) return std::nullopt;
        return it->second;
    }

    void ForgetLearning(const std::u32string& reading, const std::u32string& engineResult,
                        const std::u32string& shown) override {
        forgotten.push_back({reading, engineResult, shown});
    }

    void PunctuationChanged(const std::u32string& value) override { punctuation = value; }

    std::vector<std::u32string> CandidateOnlyWords(const std::u32string& reading) override {
        const auto it = candidateOnly.find(reading);
        return it == candidateOnly.end() ? std::vector<std::u32string>() : it->second;
    }

    std::vector<iroha::InputHistoryEntry> PredictionCandidates(const std::u32string& prefix) override {
        std::vector<iroha::InputHistoryEntry> found;
        for (const auto& entry : history) {
            if (entry.reading.size() > prefix.size() && entry.reading.compare(0, prefix.size(), prefix) == 0) {
                found.push_back(entry);
            }
        }
        return found;
    }

    void RecordInputHistory(const std::u32string& reading, const std::u32string& text) override {
        recordedHistory.push_back(reading + U"→" + text);
    }

    void PrivateModeChanged(bool enabled) override { privateModeChanges.push_back(enabled); }

    void RecordConversion(const iroha::ConversionLogEntry& entry) override { conversions.push_back(entry); }

    bool TypoCorrectionAvailable() override { return typoAvailable; }
    void RequestTypoCorrection(uint64_t generation, const std::u32string& reading, int delayMs, double threshold,
                               size_t minimumLength) override {
        (void)threshold;
        typoRequests.push_back({generation, reading, delayMs, minimumLength});
    }
    void CancelTypoCorrection() override { ++typoCancels; }
    void RequestAI(uint64_t generation, size_t preset, const std::u32string& text) override {
        aiRequests.push_back({generation, preset, text});
    }
    void CancelAI() override { ++aiCancels; }
    void RequestAIFixCheck(uint64_t generation, const std::u32string& reading, const std::u32string& context,
                           int delayMs) override {
        (void)delayMs;
        aiFixChecks.push_back({generation, reading, context});
    }
    void RequestAIFix(uint64_t generation, const iroha::AIRequest& request) override {
        aiFixRequests.push_back({generation, request});
    }
    std::optional<iroha::TypoCorrection> CorrectTypo(const std::u32string& reading, double threshold,
                                                     size_t minimumLength) override {
        (void)threshold;
        (void)minimumLength;
        ++correctTypoCalls;
        const auto it = typoTable.find(reading);
        if (it == typoTable.end()) return std::nullopt;
        return iroha::TypoCorrection{reading, it->second, 5};
    }

    // 読み → 変換結果（候補数1）、読み → 候補の一覧（候補数2以上）
    std::map<std::u32string, std::u32string> table;
    std::map<std::u32string, std::vector<std::u32string>> candidateTable;
    // (読み, 表示) → 学習が差し替える前のエンジンの結果
    std::map<std::pair<std::u32string, std::u32string>, std::u32string> engineResults;
    bool shouldFail = false;
    std::vector<LiveRequest> liveRequests;
    int liveCancels = 0;
    std::vector<ConvertCall> convertCalls;
    std::vector<Commit> commits;
    std::vector<std::tuple<std::u32string, std::u32string, std::u32string>> forgotten;
    std::u32string punctuation;
    // 読み → ユーザ辞書の候補ウィンドウ専用の語
    std::map<std::u32string, std::vector<std::u32string>> candidateOnly;
    std::vector<iroha::InputHistoryEntry> history;
    std::vector<std::u32string> recordedHistory;
    std::vector<bool> privateModeChanges;
    std::vector<iroha::ConversionLogEntry> conversions;
    // 打ち間違いの訂正: 使えるか・頼まれた休止の訂正・取り消し・その場の訂正の回数・読み → 直した読み
    struct TypoRequest {
        uint64_t generation;
        std::u32string reading;
        int delayMs;
        size_t minimumLength;
    };
    bool typoAvailable = false;
    std::vector<TypoRequest> typoRequests;
    int typoCancels = 0;
    int correctTypoCalls = 0;
    std::map<std::u32string, std::u32string> typoTable;
    // AI変換: 頼まれた依頼と取り消し
    struct AIRequest {
        uint64_t generation;
        size_t preset;
        std::u32string text;
    };
    std::vector<AIRequest> aiRequests;
    int aiCancels = 0;
    // AIで直す: 頼まれた休止の判定・AI への依頼
    struct AIFixCheck {
        uint64_t generation;
        std::u32string reading;
        std::u32string context;
    };
    struct AIFixRequest {
        uint64_t generation;
        iroha::AIRequest request;
    };
    std::vector<AIFixCheck> aiFixChecks;
    std::vector<AIFixRequest> aiFixRequests;

    // 「こえ」モード: 頼まれた録音の番号・やめた回数・取り消し・始められないときの理由
    std::u32string RequestVoiceStart(uint64_t generation) override {
        voiceStarts.push_back(generation);
        return voiceStartError;
    }
    void RequestVoiceStop(uint64_t generation) override { voiceStops.push_back(generation); }
    void CancelVoice() override { ++voiceCancels; }
    std::vector<uint64_t> voiceStarts;
    std::vector<uint64_t> voiceStops;
    int voiceCancels = 0;
    std::u32string voiceStartError;
};

using Key = iroha::SessionKey;
using Strings = std::vector<std::u32string>;

iroha::SessionOutput Type(iroha::InputSession& session, const std::u32string& keys) {
    iroha::SessionOutput output;
    for (char32_t c : keys) {
        // 大文字は Shift を押して打ったものとして送る（TIP と同じ）
        output = session.Handle(Key::Character, c, c >= U'A' && c <= U'Z', nullptr);
    }
    return output;
}

iroha::SessionOutput Press(iroha::InputSession& session, Key key, bool shift = false) {
    return session.Handle(key, 0, shift, nullptr);
}

// 最後に頼まれた変換に結果を渡す
bool CompleteLatest(iroha::InputSession& session, FakeSessionHost& host,
                    const std::u32string& converted) {
    const auto& request = host.liveRequests.back();
    return session.CompleteLiveConversion(request.generation, request.reading, &converted);
}

size_t SingleConvertCalls(const FakeSessionHost& host) {
    size_t count = 0;
    for (const auto& call : host.convertCalls) count += call.count == 1 ? 1 : 0;
    return count;
}

// 入力中はかなを出し、ライブ変換が届いたら変換結果に置き換える
void TestSessionLiveConversionReplacesKana() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    auto output = Type(session, U"kyou");
    EXPECT_EQ(output.PreeditText(), U"きょう");
    EXPECT_TRUE(output.livePending && output.Composing() && !output.segmenting);
    EXPECT_EQ(host.liveRequests.back().reading, U"きょう");

    const uint64_t before = session.Revision();
    EXPECT_TRUE(CompleteLatest(session, host, U"今日"));
    EXPECT_TRUE(session.Revision() > before);
    output = session.Current();
    EXPECT_EQ(output.PreeditText(), U"今日");
    EXPECT_TRUE(!output.livePending);
}

// 読みが伸びたら、前の変換結果に新しいかなを足して見せる（結果が届いたら置き換わる）
void TestSessionKeepsPreviousConversionAsPrefix() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    Type(session, U"kyou");
    CompleteLatest(session, host, U"今日");
    auto output = Type(session, U"h");
    EXPECT_EQ(output.PreeditText(), U"今日h"); // 未解決のローマ字
    output = Type(session, U"a");
    EXPECT_EQ(output.PreeditText(), U"今日は");
    EXPECT_TRUE(output.livePending);
    EXPECT_EQ(host.liveRequests.back().reading, U"きょうは");
    CompleteLatest(session, host, U"今日は");
    EXPECT_EQ(session.Current().PreeditText(), U"今日は");
}

// 読みが伸びたあとに届いた前の依頼の結果は、頭の部分の変換として使う（最新の結果はまだ待つ）
void TestSessionUsesEarlierLiveResultAsPrefix() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    Type(session, U"kyou");
    const auto earlier = host.liveRequests.back();
    Type(session, U"ha");
    const std::u32string converted = U"今日";
    EXPECT_TRUE(session.CompleteLiveConversion(earlier.generation, earlier.reading, &converted));
    EXPECT_EQ(session.Current().PreeditText(), U"今日は");
    EXPECT_TRUE(session.Current().livePending);
    // 確定も表示のとおり
    EXPECT_EQ(Press(session, Key::Enter).commit, U"今日は");
}

// 要らなくなった依頼（文字を消す前・確定の前のもの）の結果は捨てる
void TestSessionIgnoresCancelledLiveResult() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    Type(session, U"kyou");
    const auto cancelled = host.liveRequests.back();
    EXPECT_TRUE(host.liveCancels == 0);
    Press(session, Key::Backspace);
    EXPECT_TRUE(host.liveCancels == 1);
    const std::u32string converted = U"今日";
    EXPECT_TRUE(
        !session.CompleteLiveConversion(cancelled.generation, cancelled.reading, &converted));
    EXPECT_EQ(session.Current().PreeditText(), U"きょ");

    // 確定したあとに届いた結果が、次の合成に混ざらない
    Type(session, U"u");
    const auto beforeCommit = host.liveRequests.back();
    Press(session, Key::Enter);
    Type(session, U"kyou");
    EXPECT_TRUE(!session.CompleteLiveConversion(beforeCommit.generation, beforeCommit.reading,
                                                &converted));
    EXPECT_EQ(session.Current().PreeditText(), U"きょう");
    EXPECT_TRUE(session.Current().livePending);
}

// 未解決のローマ字が増えただけなら、頼んである変換をそのまま待つ
void TestSessionKeepsPendingRequestForSameReading() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    Type(session, U"kyou");
    const size_t requests = host.liveRequests.size();
    const auto output = Type(session, U"h");
    EXPECT_TRUE(host.liveRequests.size() == requests && host.liveCancels == 0);
    EXPECT_TRUE(output.livePending);
    EXPECT_TRUE(CompleteLatest(session, host, U"今日"));
    EXPECT_EQ(session.Current().PreeditText(), U"今日h");
}

// アプリのテキストを読めなかったときは、直前に確定した文字列を左文脈に使う
void TestSessionFallsBackToCommittedContext() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    const std::u32string context = U"前の文。";
    session.Handle(Key::Character, U'k', false, &context);
    Type(session, U"you");
    CompleteLatest(session, host, U"今日");
    Press(session, Key::Enter);
    Type(session, U"ha"); // 左文脈なし
    EXPECT_EQ(host.liveRequests.back().context, U"前の文。今日");
}

// 変換済みの読みに戻ったときは変換し直さない
void TestSessionSkipsConversionForSameReading() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    Type(session, U"kyou");
    CompleteLatest(session, host, U"今日");
    const size_t requests = host.liveRequests.size();
    // 未解決のローマ字が増えても読み（確定したかな）は同じ
    const auto output = Type(session, U"k");
    EXPECT_TRUE(host.liveRequests.size() == requests);
    EXPECT_TRUE(!output.livePending);
    EXPECT_EQ(output.PreeditText(), U"今日k");
}

void TestSessionEnterCommitsLiveResult() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    Type(session, U"kyouha");
    CompleteLatest(session, host, U"今日は");
    const auto output = Press(session, Key::Enter);
    EXPECT_EQ(output.commit, U"今日は");
    EXPECT_TRUE(!output.Composing() && !output.livePending);
    Press(session, Key::Reset);
    EXPECT_TRUE(host.commits.empty()); // ライブ変換の確定は学習しない
}

// 変換が追いつく前の確定は、表示と同じ「変換済みの部分 + 追加のかな」
void TestSessionEnterBeforeConversionCatchesUp() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    Type(session, U"kyou");
    CompleteLatest(session, host, U"今日");
    Type(session, U"ha");
    EXPECT_EQ(Press(session, Key::Enter).commit, U"今日は");

    // 1度も変換が届いていなければかなのまま
    Type(session, U"kyou");
    EXPECT_EQ(Press(session, Key::Enter).commit, U"きょう");
}

// 確定のとき未解決のローマ字（n）はかなにして、変換結果の後ろに付ける
void TestSessionEnterFlushesPendingRomaji() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    Type(session, U"kyou");
    CompleteLatest(session, host, U"今日");
    EXPECT_EQ(Type(session, U"n").PreeditText(), U"今日n");
    EXPECT_EQ(Press(session, Key::Enter).commit, U"今日ん");
}

// 文字を消したらかな表示に戻し、次の文字でライブ変換を再開する
void TestSessionBackspaceShowsKana() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    Type(session, U"kyouha");
    CompleteLatest(session, host, U"今日は");
    auto output = Press(session, Key::Backspace);
    EXPECT_EQ(output.PreeditText(), U"きょう");
    EXPECT_TRUE(!output.livePending);

    output = Type(session, U"ha");
    // 変換済みの読みに戻ったので、すぐ変換結果を出す
    EXPECT_EQ(output.PreeditText(), U"今日は");

    Press(session, Key::Backspace);
    EXPECT_EQ(Press(session, Key::Enter).commit, U"きょう"); // 消した直後の確定はかな

    // 最後の1文字を消すと合成が終わる
    Type(session, U"a");
    output = Press(session, Key::Backspace);
    EXPECT_TRUE(!output.Composing());
}

// 1回目のEscでかなに戻し、2回目で入力を取り消す
void TestSessionEscape() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    Type(session, U"kyou");
    CompleteLatest(session, host, U"今日");
    auto output = Press(session, Key::Escape);
    EXPECT_EQ(output.PreeditText(), U"きょう");
    output = Press(session, Key::Escape);
    EXPECT_TRUE(!output.Composing());
    EXPECT_TRUE(output.commit.empty());
}

// Space で文節に分け、最初の文節の候補ウィンドウまで開く（macOS版と同じく 1 回の Space で開く）。
// ライブ変換が同じ読みを変換済みならその結果を使う
void TestSessionSpaceEntersSegmentModeAndOpensCandidates() {
    FakeSessionHost host;
    host.candidateTable[U"きょうはいい"] = {U"今日はいい", U"京はいい"};
    iroha::InputSession session(&host);
    Type(session, U"kyouhaiitenki");
    CompleteLatest(session, host, U"今日はいい天気");
    const auto output = Press(session, Key::Space);
    EXPECT_TRUE(output.segmenting && output.candidatesVisible);
    EXPECT_TRUE(SingleConvertCalls(host) == 0); // 全体の変換はしない
    EXPECT_TRUE(host.convertCalls.size() == 1 && host.convertCalls[0].count == 8);
    EXPECT_TRUE(output.candidateIndex == 0);
    EXPECT_TRUE(output.preedit.size() == 2);
    if (output.preedit.size() == 2) {
        EXPECT_EQ(output.preedit[0].text, U"今日はいい");
        EXPECT_TRUE(output.preedit[0].style == iroha::SessionOutput::kSelectedSegment);
        EXPECT_EQ(output.preedit[1].text, U"天気");
        EXPECT_TRUE(output.preedit[1].style == iroha::SessionOutput::kInput);
    }
}

// ライブ変換がまだ届いていなければ、Space でその場で変換する（文脈つき）
void TestSessionSpaceConvertsWhenNoLiveResult() {
    FakeSessionHost host;
    host.table[U"きしゃ"] = U"記者";
    iroha::InputSession session(&host);
    const std::u32string context = U"昨日の";
    session.Handle(Key::Character, U'k', false, &context);
    Type(session, U"isha");
    const auto output = Press(session, Key::Space);
    EXPECT_TRUE(output.segmenting);
    EXPECT_EQ(output.PreeditText(), U"記者");
    EXPECT_TRUE(!host.convertCalls.empty());
    if (!host.convertCalls.empty()) {
        EXPECT_EQ(host.convertCalls[0].reading, U"きしゃ");
        EXPECT_EQ(host.convertCalls[0].context, U"昨日の");
        EXPECT_TRUE(host.convertCalls[0].count == 1);
    }
    // 頼んであったライブ変換の結果はもう受け取らない
    const auto request = host.liveRequests.back();
    const std::u32string late = U"汽車";
    EXPECT_TRUE(!session.CompleteLiveConversion(request.generation, request.reading, &late));
}

// 変換できないときは読みのまま表示を続ける
void TestSessionSpaceKeepsKanaWhenConversionFails() {
    FakeSessionHost host;
    host.shouldFail = true;
    iroha::InputSession session(&host);
    Type(session, U"kisha");
    const auto output = Press(session, Key::Space);
    EXPECT_TRUE(!output.segmenting);
    EXPECT_EQ(output.PreeditText(), U"きしゃ");
    EXPECT_EQ(Press(session, Key::Enter).commit, U"きしゃ");
}

// 候補を選んで確定すると、読み・直す前の結果・確定した文字列を学習に知らせる。
// 知らせるのは確定を取り消せなくなったとき（次のキー）
void TestSessionCandidatesAndCommit() {
    FakeSessionHost host;
    host.table[U"きしゃ"] = U"記者";
    host.candidateTable[U"きしゃ"] = {U"記者", U"汽車", U"貴社"};
    iroha::InputSession session(&host);
    Type(session, U"kisha");

    auto output = Press(session, Key::Space); // 文節に分けて候補ウィンドウを開く（今の表示を選んだ状態）
    EXPECT_TRUE(output.candidatesVisible);
    // 末尾にひらがな・カタカナ
    EXPECT_TRUE(output.candidates == (Strings{U"記者", U"汽車", U"貴社", U"きしゃ", U"キシャ"}));
    EXPECT_TRUE(output.candidateIndex == 0);
    output = Press(session, Key::Space); // 次の候補
    EXPECT_TRUE(output.candidateIndex == 1);
    EXPECT_EQ(output.PreeditText(), U"汽車");

    output = Press(session, Key::Up);
    EXPECT_TRUE(output.candidateIndex == 0);
    output = Press(session, Key::Space, true); // Shift+Space は前の候補（先頭からは末尾へ）
    EXPECT_TRUE(output.candidateIndex == 4);
    output = session.Handle(Key::Character, U'2', false, nullptr); // 数字キーで選ぶと候補ウィンドウを閉じる
    EXPECT_TRUE(!output.candidatesVisible && output.segmenting);
    EXPECT_EQ(output.PreeditText(), U"汽車");

    output = Press(session, Key::Enter);
    EXPECT_EQ(output.commit, U"汽車");
    EXPECT_TRUE(!output.Composing() && !output.candidatesVisible);
    EXPECT_TRUE(host.commits.empty());
    Press(session, Key::Reset);
    EXPECT_TRUE(host.commits.size() == 1);
    if (host.commits.size() == 1) {
        EXPECT_EQ(host.commits[0].reading, U"きしゃ");
        EXPECT_EQ(host.commits[0].baseline, U"記者");
        EXPECT_EQ(host.commits[0].engineBaseline, U"記者");
        EXPECT_EQ(host.commits[0].committed, U"汽車");
    }
}

// 今の結果は候補の先頭に置く（一覧に無ければ足し、あれば先頭へ移す）
void TestSessionKeepsCurrentResultInCandidates() {
    FakeSessionHost host;
    host.table[U"きしゃ"] = U"喜捨";
    host.candidateTable[U"きしゃ"] = {U"記者", U"汽車"};
    iroha::InputSession session(&host);
    Type(session, U"kisha");
    auto output = Press(session, Key::Space);
    EXPECT_TRUE(output.candidates == (Strings{U"喜捨", U"記者", U"汽車", U"きしゃ", U"キシャ"}));
    output = Press(session, Key::Down);
    EXPECT_TRUE(output.candidateIndex == 1);
    Press(session, Key::Escape);
    Press(session, Key::Escape);
    Press(session, Key::Escape);

    host.table[U"きしゃ"] = U"汽車";
    Type(session, U"kisha");
    output = Press(session, Key::Space);
    EXPECT_TRUE(output.candidates == (Strings{U"汽車", U"記者", U"きしゃ", U"キシャ"}));
}

// 記号・顔文字・絵文字を候補に合流させ、それを選んだ確定は学習に知らせない
void TestSessionSymbolCandidates() {
    FakeSessionHost host;
    host.table[U"わらい"] = U"笑い";
    host.candidateTable[U"わらい"] = {U"笑い", U"嗤い"};
    iroha::InputSession session(&host);
    session.SetSymbolTable(std::make_shared<iroha::SymbolCandidateTable>(
        "", "(^^)\tわらい\tSMILE", "", "1F602\t😂\twarai わらい\t\t\t\tE0.6"));
    Type(session, U"warai");
    auto output = Press(session, Key::Space);
    // 絵文字は 7 番目だが、候補が少なければエンジンの候補の後ろ。顔文字はその後、最後にひらがな・カタカナ
    EXPECT_TRUE(output.candidates == (Strings{U"笑い", U"嗤い", U"😂", U"(^^)", U"わらい", U"ワライ"}));
    Press(session, Key::Down);
    output = Press(session, Key::Down);
    EXPECT_EQ(output.PreeditText(), U"😂");
    EXPECT_EQ(Press(session, Key::Enter).commit, U"😂");
    Press(session, Key::Reset);
    EXPECT_TRUE(host.commits.empty());

    // 表に無い候補を選んだ確定は学習に知らせる
    Type(session, U"warai");
    Press(session, Key::Space);
    Press(session, Key::Down);
    EXPECT_EQ(Press(session, Key::Enter).commit, U"嗤い");
    Press(session, Key::Reset);
    EXPECT_TRUE(host.commits.size() == 1);
}

// 専門辞書の語になっている候補に、辞書の名前の印を付ける（印の無い候補は空）
void TestSessionTermAnnotations() {
    FakeSessionHost host;
    host.table[U"ちらくが"] = U"散楽が";
    host.candidateTable[U"ちらくが"] = {U"散楽が", U"地絡が"};
    iroha::InputSession session(&host);
    iroha::TermDictionaryLookup::Table table;
    table.label = U"工学";
    table.data = "# 注記\nちらく\t地絡\n";
    table.start = iroha::TermDictionaryLookup::BodyStart(table.data);
    session.SetTermLookup(std::make_shared<iroha::TermDictionaryLookup>(
        std::vector<iroha::TermDictionaryLookup::Table>{table}));
    Type(session, U"chirakuga");
    const auto output = Press(session, Key::Space);
    EXPECT_TRUE(output.candidates == (Strings{U"散楽が", U"地絡が", U"ちらくが", U"チラクガ"}));
    EXPECT_TRUE(output.candidateAnnotations == (Strings{U"", U"工学", U"", U""}));

    // 印の付く候補が無ければ空にする
    iroha::InputSession plain(&host);
    Type(plain, U"chirakuga");
    EXPECT_TRUE(Press(plain, Key::Space).candidateAnnotations.empty());
}

// 変換ルールの出力を今の表示の直後に置く。ルールの出力・候補ウィンドウ専用の語を含む候補を選んだ確定は学習しない
void TestSessionRewriteRulesAndCandidateOnlyWords() {
    FakeSessionHost host;
    host.table[U"きょう"] = U"今日";
    host.candidateTable[U"きょう"] = {U"今日", U"京", U"#今日の一枚"};
    host.candidateOnly[U"きょう"] = {U"#今日の一枚"};
    iroha::InputSession session(&host);
    session.SetRewriteRules(std::make_shared<iroha::UserRewriteRuleSet>(
        std::vector<iroha::UserRewriteRule>{iroha::UserRewriteRule::Make(U"きょう", U"{{date}}")}));
    const std::u32string today = iroha::RewriteTemplate(U"{{date}}").Render(iroha::RewriteContext::Now());
    Type(session, U"kyou");
    auto output = Press(session, Key::Space);
    EXPECT_TRUE(output.candidates == (Strings{U"今日", today, U"京", U"#今日の一枚", U"きょう", U"キョウ"}));
    output = Press(session, Key::Down);
    EXPECT_EQ(output.PreeditText(), today);
    EXPECT_EQ(Press(session, Key::Enter).commit, today);
    Press(session, Key::Reset);
    EXPECT_TRUE(host.commits.empty());

    Type(session, U"kyou");
    Press(session, Key::Space);
    Press(session, Key::Down);
    Press(session, Key::Down);
    EXPECT_EQ(Press(session, Key::Down).PreeditText(), U"#今日の一枚");
    Press(session, Key::Enter);
    Press(session, Key::Reset);
    EXPECT_TRUE(host.commits.empty());

    Type(session, U"kyou");
    Press(session, Key::Space);
    Press(session, Key::Down);
    EXPECT_EQ(Press(session, Key::Down).PreeditText(), U"京");
    Press(session, Key::Enter);
    Press(session, Key::Reset);
    EXPECT_TRUE(host.commits.size() == 1);
}

// 文節の移動と、文節の候補を求めるときの文脈（左側の変換結果を足す）
void TestSessionMoveSegment() {
    FakeSessionHost host;
    host.table[U"きょうはいいてんき"] = U"今日はいい天気";
    host.candidateTable[U"てんき"] = {U"天気", U"転機"};
    iroha::InputSession session(&host);
    const std::u32string context = U"たぶん";
    session.Handle(Key::Character, U'k', false, &context);
    Type(session, U"youhaiitenki");
    Press(session, Key::Space);

    auto output = Press(session, Key::Right); // 文節を移ると候補ウィンドウは閉じる
    EXPECT_TRUE(!output.candidatesVisible);
    EXPECT_TRUE(output.preedit.size() == 2 &&
                output.preedit[1].style == iroha::SessionOutput::kSelectedSegment);
    output = Press(session, Key::Right); // 末尾より右へは動かない
    EXPECT_TRUE(output.preedit[1].style == iroha::SessionOutput::kSelectedSegment);

    output = Press(session, Key::Space); // 開く
    EXPECT_EQ(host.convertCalls.back().reading, U"てんき");
    EXPECT_EQ(host.convertCalls.back().context, U"たぶん今日はいい");
    EXPECT_TRUE(host.convertCalls.back().count == 8);
    EXPECT_TRUE(output.candidatesVisible && output.candidateIndex == 0);
    output = Press(session, Key::Space); // 次の候補
    EXPECT_EQ(output.PreeditText(), U"今日はいい転機");

    output = Press(session, Key::Left);
    EXPECT_TRUE(!output.candidatesVisible);
    EXPECT_EQ(Press(session, Key::Enter).commit, U"今日はいい転機");
}

// Shift+矢印で文節の区切りを動かし、その文節と後ろを変換し直す
void TestSessionResizeSegment() {
    FakeSessionHost host;
    host.table[U"きょうはいいてんき"] = U"今日はいい天気";
    host.table[U"きょうはいいて"] = U"今日はいい手";
    host.table[U"んき"] = U"ンキ";
    iroha::InputSession session(&host);
    Type(session, U"kyouhaiitenki");
    Press(session, Key::Space);
    const auto output = Press(session, Key::Right, true);
    EXPECT_EQ(output.PreeditText(), U"今日はいい手ンキ");
    EXPECT_TRUE(output.preedit.size() == 2);
    // 読み全体は変わらない（学習の読みは入力全体）
    Press(session, Key::Enter);
    Press(session, Key::Reset);
    EXPECT_TRUE(host.commits.size() == 1 && host.commits[0].reading == U"きょうはいいてんき");
}

// Esc は候補ウィンドウを閉じ、もう 1 度で変換をやめて読み（かな）に戻る
void TestSessionEscapeFromSegmentMode() {
    FakeSessionHost host;
    host.table[U"きしゃ"] = U"記者";
    iroha::InputSession session(&host);
    Type(session, U"kisha");
    auto output = Press(session, Key::Space);
    EXPECT_TRUE(output.candidatesVisible);
    output = Press(session, Key::Escape);
    EXPECT_TRUE(output.segmenting && !output.candidatesVisible);
    output = Press(session, Key::Escape);
    EXPECT_TRUE(!output.segmenting);
    EXPECT_EQ(output.PreeditText(), U"きしゃ");
    Press(session, Key::Reset);
    EXPECT_TRUE(host.commits.empty());
    // もう1度 Space を押せば全体を変換し直さずに文節変換へ戻れる
    Type(session, U"kisha");
    Press(session, Key::Space);
    Press(session, Key::Escape);
    Press(session, Key::Escape);
    const size_t calls = SingleConvertCalls(host);
    output = Press(session, Key::Space);
    EXPECT_TRUE(output.segmenting && SingleConvertCalls(host) == calls);
    EXPECT_EQ(output.PreeditText(), U"記者");
}

// 文節変換中に文字を打つと、選んでいる文節をその文字から打ち直す（範囲の読み全体を裏で変換し直す）。
// 打ち直した確定は学習しない。大文字（英字入力）は確定して新しい入力を始める
void TestSessionRetypeSegment() {
    FakeSessionHost host;
    host.table[U"こどものこうえん"] = U"子供の公演";
    iroha::InputSession session(&host);
    Type(session, U"kodomonokouenn");
    Press(session, Key::Space);
    auto output = Type(session, U"d");
    EXPECT_EQ(output.PreeditText(), U"d公演"); // 打ち直しの読み（未解決のローマ字）を前の文節の位置に
    EXPECT_TRUE(output.segmenting && !output.candidatesVisible);
    output = Type(session, U"aigakuno");
    EXPECT_EQ(output.PreeditText(), U"だいがくの公演");
    EXPECT_TRUE(output.livePending);
    EXPECT_EQ(host.liveRequests.back().reading, U"だいがくのこうえん");
    CompleteLatest(session, host, U"大学の講演");
    output = session.Current();
    EXPECT_EQ(output.PreeditText(), U"大学の講演");
    EXPECT_EQ(Press(session, Key::Enter).commit, U"大学の講演");
    Press(session, Key::Reset);
    EXPECT_TRUE(host.commits.empty());

    host.table[U"きしゃ"] = U"記者";
    Type(session, U"kisha");
    Press(session, Key::Space);
    output = Type(session, U"A");
    EXPECT_EQ(output.commit, U"記者");
    EXPECT_EQ(output.PreeditText(), U"A");
}

// 文節変換中の Backspace は、選んでいる文節の読みを後ろから消す（Esc で打ち直す前に戻す）
void TestSessionSegmentBackspace() {
    FakeSessionHost host;
    host.table[U"きょうはいいてんき"] = U"今日はいい天気";
    iroha::InputSession session(&host);
    Type(session, U"kyouhaiitenki");
    Press(session, Key::Space);
    Press(session, Key::Right);
    auto output = Press(session, Key::Backspace);
    EXPECT_EQ(output.PreeditText(), U"今日はいいてん");
    output = Press(session, Key::Escape);
    EXPECT_EQ(output.PreeditText(), U"今日はいい天気");

    Press(session, Key::Backspace);
    Press(session, Key::Backspace);
    output = Press(session, Key::Backspace);
    EXPECT_EQ(output.PreeditText(), U"今日はいい");
    // 空になった文節は確定するときに消える
    EXPECT_EQ(Press(session, Key::Enter).commit, U"今日はいい");

    // 最後の文節まで消すと文節変換を終える
    host.table[U"きしゃ"] = U"記者";
    Type(session, U"kisha");
    Press(session, Key::Space);
    Press(session, Key::Backspace);
    Press(session, Key::Backspace);
    output = Press(session, Key::Backspace);
    EXPECT_TRUE(!output.Composing() && !output.segmenting);
}

// 左文脈はカーソルのある行だけ・行頭の空白を除く・末尾40文字。合成の途中に届いたものは使わない
void TestSessionLeftContext() {
    using Session = iroha::InputSession;
    EXPECT_EQ(Session::NormalizeLeftContext(U"前の段落。\n次の段落の"), U"次の段落の");
    EXPECT_EQ(Session::NormalizeLeftContext(U"a\r\nb"), U"b");
    EXPECT_EQ(Session::NormalizeLeftContext(U"前の行。\n"), U"");
    EXPECT_EQ(Session::NormalizeLeftContext(U"前の行\n　  字下げした行の"), U"字下げした行の");
    EXPECT_EQ(Session::NormalizeLeftContext(U"iroha, azookeyは"), U"iroha, azookeyは");
    EXPECT_TRUE(Session::NormalizeLeftContext(std::u32string(50, U'あ')).size() == 40);

    FakeSessionHost host;
    Session session(&host);
    const std::u32string first = U"前の行\n今の行";
    session.Handle(Key::Character, U'a', false, &first);
    EXPECT_EQ(host.liveRequests.back().context, U"今の行");
    const std::u32string later = U"別の文脈";
    session.Handle(Key::Character, U'i', false, &later);
    EXPECT_EQ(host.liveRequests.back().context, U"今の行");
}

// CommitNow は今の表示で確定し、Reset は何も確定せずに捨てる
void TestSessionCommitNowAndReset() {
    FakeSessionHost host;
    host.table[U"きしゃ"] = U"記者";
    iroha::InputSession session(&host);
    Type(session, U"kyou");
    CompleteLatest(session, host, U"今日");
    EXPECT_EQ(Press(session, Key::CommitNow).commit, U"今日");
    EXPECT_TRUE(!session.IsComposing());

    Type(session, U"kisha");
    Press(session, Key::Space);
    EXPECT_EQ(Press(session, Key::CommitNow).commit, U"記者");

    Type(session, U"kyou");
    const auto output = Press(session, Key::Reset);
    EXPECT_TRUE(output.commit.empty() && !output.Composing() && !output.livePending);
    EXPECT_TRUE(Press(session, Key::CommitNow).commit.empty());
}

// ライブ変換を使わない設定では、Space を押すまでかなのまま
void TestSessionLiveConversionDisabled() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    session.SetLiveConversionEnabled(false);
    const auto output = Type(session, U"kyou");
    EXPECT_TRUE(host.liveRequests.empty() && !output.livePending);
    EXPECT_EQ(output.PreeditText(), U"きょう");
    EXPECT_EQ(Press(session, Key::Enter).commit, U"きょう");
}

// F6〜F10（Ctrl+U/I/O/P/T）で表示形を指定する。続けて打つと、指定した部分は固定され、残りだけを変換する
void TestSessionDisplayConversion() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    Type(session, U"kisha");
    CompleteLatest(session, host, U"記者");
    EXPECT_EQ(Press(session, Key::DisplayKatakana).PreeditText(), U"キシャ");
    EXPECT_EQ(Press(session, Key::DisplayHalfKatakana).PreeditText(), U"ｷｼｬ");
    EXPECT_EQ(Press(session, Key::DisplayFullAlphanumeric).PreeditText(), U"ｋｉｓｈａ");
    EXPECT_EQ(Press(session, Key::DisplayHalfAlphanumeric).PreeditText(), U"kisha");
    EXPECT_EQ(Press(session, Key::DisplayHiragana).PreeditText(), U"きしゃ");
    // Esc で指定をやめてかなに戻る
    Press(session, Key::DisplayKatakana);
    EXPECT_EQ(Press(session, Key::Escape).PreeditText(), U"きしゃ");

    Press(session, Key::DisplayKatakana);
    auto output = Type(session, U"desu");
    EXPECT_EQ(output.PreeditText(), U"キシャです");
    EXPECT_EQ(host.liveRequests.back().reading, U"です");
    EXPECT_EQ(host.liveRequests.back().context, U"キシャ");
    CompleteLatest(session, host, U"です");
    EXPECT_EQ(Press(session, Key::Enter).commit, U"キシャです");

    // 消していって固定部分に戻ると、読みに戻して消す
    Type(session, U"kisha");
    Press(session, Key::DisplayKatakana);
    Type(session, U"a");
    Press(session, Key::Backspace);
    output = Press(session, Key::Backspace);
    EXPECT_EQ(output.PreeditText(), U"きし");
}

// 学習で差し替えた表示を、表示形の指定で退けて確定したら、その学習を消す
void TestSessionDisplayConversionForgetsLearning() {
    FakeSessionHost host;
    host.engineResults[{U"は", U"派"}] = U"は";
    iroha::InputSession session(&host);
    Type(session, U"ha");
    CompleteLatest(session, host, U"派");
    Press(session, Key::DisplayHiragana);
    EXPECT_EQ(Press(session, Key::Enter).commit, U"は");
    EXPECT_TRUE(host.forgotten.empty()); // 取り消せる間は消さない
    Press(session, Key::Reset);
    EXPECT_TRUE(host.forgotten.size() == 1);
    if (host.forgotten.size() == 1) {
        EXPECT_EQ(std::get<0>(host.forgotten[0]), U"は");
        EXPECT_EQ(std::get<1>(host.forgotten[0]), U"は");
        EXPECT_EQ(std::get<2>(host.forgotten[0]), U"派");
    }
}

// Shift+英字で英字入力を始め、Shift を離して打つとかな入力に戻る（それまでの英字は固定部分）
void TestSessionAlphabetRun() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    Type(session, U"kyouha");
    CompleteLatest(session, host, U"今日は");
    auto output = Type(session, U"AI");
    EXPECT_EQ(output.PreeditText(), U"今日はAI");
    // Shift を押している間は記号もそのまま英字に足す
    output = session.Handle(Key::Character, U'!', true, nullptr);
    EXPECT_EQ(output.PreeditText(), U"今日はAI!");
    Press(session, Key::Backspace);
    output = Type(session, U"wo");
    EXPECT_EQ(output.PreeditText(), U"今日はAIを");
    EXPECT_EQ(host.liveRequests.back().reading, U"を");
    EXPECT_EQ(host.liveRequests.back().context, U"今日はAI");

    // 消していくと英字に戻り、英字を消し切るとかなに戻る（英字を打つ前の変換の表示）
    Press(session, Key::Backspace);
    output = Press(session, Key::Backspace);
    EXPECT_EQ(output.PreeditText(), U"今日はA");
    output = Press(session, Key::Backspace);
    EXPECT_EQ(output.PreeditText(), U"今日は");
    EXPECT_EQ(Press(session, Key::Enter).commit, U"今日は");

    // Esc は英字入力を取り消して、英字を打つ前の表示に戻す
    Type(session, U"kyouha");
    CompleteLatest(session, host, U"今日は");
    Type(session, U"AB");
    EXPECT_EQ(Press(session, Key::Escape).PreeditText(), U"今日は");
    Press(session, Key::Escape);
    Press(session, Key::Escape);

    // 英字だけで Space を押すと、英字の文節の候補（大文字・小文字・全角）を出す
    Type(session, U"AI");
    output = Press(session, Key::Space);
    EXPECT_TRUE(output.segmenting && output.candidatesVisible);
    EXPECT_TRUE(output.candidates == (Strings{U"AI", U"ai", U"Ai", U"ＡＩ"}));
    output = Press(session, Key::Down);
    EXPECT_EQ(output.PreeditText(), U"ai");
}

// Ctrl+. で句読点の組を切り替え、設定に知らせる
void TestSessionTogglePunctuation() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    Press(session, Key::TogglePunctuation);
    EXPECT_EQ(host.punctuation, U"，．");
    EXPECT_EQ(Type(session, U",.").PreeditText(), U"，．");
    Press(session, Key::Escape);
    Press(session, Key::Escape);
    Press(session, Key::TogglePunctuation);
    EXPECT_EQ(host.punctuation, U"、。");
    EXPECT_EQ(Type(session, U",.").PreeditText(), U"、。");
}

// 入力していないときの Space は全角スペース（設定で半角にできる。Shift+Space は送られてこない）
void TestSessionIdleSpace() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    auto output = Press(session, Key::Space);
    EXPECT_EQ(output.commit, U"　");
    EXPECT_TRUE(output.fullWidthSpace && output.undoableCommit.empty());
    iroha::SessionSettings settings;
    settings.fullWidthSpace = false;
    session.SetSettings(settings);
    output = Press(session, Key::Space);
    EXPECT_TRUE(output.commit.empty() && !output.fullWidthSpace);
}

// 確定した直後の Ctrl+Backspace で、確定する前の未確定文字列に戻す。取り消した確定は学習しない。
// ほかのキーが来たら取り消せなくなる
void TestSessionUndoCommit() {
    FakeSessionHost host;
    host.table[U"きしゃ"] = U"記者";
    host.candidateTable[U"きしゃ"] = {U"記者", U"汽車"};
    iroha::InputSession session(&host);
    Type(session, U"kisha");
    CompleteLatest(session, host, U"記者");
    auto output = Press(session, Key::Enter);
    EXPECT_EQ(output.undoableCommit, U"記者");
    output = Press(session, Key::UndoCommit);
    EXPECT_TRUE(output.replacesCommitted && output.Composing());
    EXPECT_EQ(output.PreeditText(), U"記者");
    EXPECT_TRUE(output.undoableCommit.empty());
    EXPECT_EQ(Press(session, Key::Enter).commit, U"記者");

    // 文節変換の確定を取り消すと文節変換に戻る。その確定の学習は捨て、確定し直したものを学習する
    Type(session, U"kisha");
    Press(session, Key::Space);
    Press(session, Key::Space);
    EXPECT_EQ(Press(session, Key::Enter).commit, U"汽車");
    output = Press(session, Key::UndoCommit);
    EXPECT_TRUE(output.segmenting && output.replacesCommitted);
    EXPECT_EQ(output.PreeditText(), U"汽車");
    EXPECT_TRUE(host.commits.empty());
    EXPECT_EQ(Press(session, Key::Enter).commit, U"汽車");
    Press(session, Key::Reset);
    EXPECT_TRUE(host.commits.size() == 1);

    // ほかのキーのあとは取り消せない
    Type(session, U"a");
    Press(session, Key::Enter);
    output = Type(session, U"i");
    EXPECT_TRUE(output.undoableCommit.empty());
    EXPECT_TRUE(!Press(session, Key::UndoCommit).replacesCommitted);
}

// 予測変換: 読みが 2 文字以上で入力履歴の候補を出し、Tab で選ぶ。選んだまま打つと候補が入って続きになる。
// Backspace で打っていた読みに戻す。確定した内容は入力履歴に記録する（取り消せなくなったとき）
void TestSessionPrediction() {
    FakeSessionHost host;
    host.history = {{U"もでる", U"モデル", 3, 100}, {U"もでむ", U"モデム", 1, 90}};
    iroha::InputSession session(&host);
    iroha::SessionSettings settings;
    settings.predictive = true;
    session.SetSettings(settings);
    auto output = Type(session, U"m");
    EXPECT_TRUE(output.predictions.empty());
    output = Type(session, U"ode");
    EXPECT_TRUE(output.predictions == (Strings{U"モデル", U"モデム"}) && output.predictionIndex == -1);
    output = Press(session, Key::Tab);
    EXPECT_TRUE(output.predictionIndex == 0);
    EXPECT_EQ(output.PreeditText(), U"もで"); // 選んだだけでは入れない
    output = Press(session, Key::Tab);
    EXPECT_TRUE(output.predictionIndex == 1);
    output = Press(session, Key::Tab, true); // Shift+Tab は前の候補
    EXPECT_TRUE(output.predictionIndex == 0);
    output = Type(session, U"ga"); // 選んだまま打つと候補が入り、続きになる
    EXPECT_EQ(output.PreeditText(), U"モデルが");
    EXPECT_EQ(host.liveRequests.back().reading, U"が");
    EXPECT_EQ(host.liveRequests.back().context, U"モデル");
    // Backspace で、続きを消してから予測を取り消し、打っていた読みに戻す
    Press(session, Key::Backspace);
    output = Press(session, Key::Backspace);
    EXPECT_EQ(output.PreeditText(), U"もで");

    // 確定した内容を入力履歴に記録する
    Press(session, Key::Tab);
    Type(session, U"wo");
    CompleteLatest(session, host, U"を");
    EXPECT_EQ(Press(session, Key::Enter).commit, U"モデルを");
    EXPECT_TRUE(host.recordedHistory.empty());
    Press(session, Key::Reset);
    EXPECT_TRUE(host.recordedHistory == (Strings{U"もでる→モデル", U"を→を"}));

    // 選んでいるときの Esc は選ぶのをやめる
    Type(session, U"mode");
    Press(session, Key::Tab);
    output = Press(session, Key::Escape);
    EXPECT_TRUE(output.predictionIndex == -1 && output.Composing());
}

// プライベートモード: 学習・入力履歴・予測を止める。切り替えは設定に知らせ、知らせの文を出す
void TestSessionPrivateMode() {
    FakeSessionHost host;
    host.table[U"きしゃ"] = U"記者";
    host.candidateTable[U"きしゃ"] = {U"記者", U"汽車"};
    host.history = {{U"きしゃ", U"汽車", 1, 1}};
    iroha::InputSession session(&host);
    iroha::SessionSettings settings;
    settings.predictive = true;
    session.SetSettings(settings);
    auto output = Press(session, Key::TogglePrivateMode);
    EXPECT_TRUE(output.privateMode && !output.notice.empty());
    EXPECT_TRUE(host.privateModeChanges == std::vector<bool>{true});
    output = Type(session, U"kis");
    EXPECT_TRUE(output.predictions.empty());
    Type(session, U"ha");
    Press(session, Key::Space);
    Press(session, Key::Space);
    EXPECT_EQ(Press(session, Key::Enter).commit, U"汽車");
    Press(session, Key::Reset);
    EXPECT_TRUE(host.commits.empty() && host.recordedHistory.empty());
    // モードを切ったあとの合成では、モード中に確定した文字列を左文脈に使わない
    output = Press(session, Key::TogglePrivateMode);
    EXPECT_TRUE(!output.privateMode);
    Type(session, U"a");
    EXPECT_EQ(host.liveRequests.back().context, U"");
}

// 変換記録: ライブ変換・文節変換・表示形の指定での確定を、取り消せなくなったときに書く（macOS版と同じ項目）
void TestSessionConversionLog() {
    using Entry = iroha::ConversionLogEntry;
    FakeSessionHost host;
    host.candidateTable[U"きしゃ"] = {U"記者", U"汽車"};
    iroha::InputSession session(&host);
    iroha::SessionSettings settings;
    settings.conversionLog = true;
    session.SetSettings(settings);
    const std::u32string context = U"駅で";

    // ライブ変換の表示をそのまま確定
    session.Handle(Key::Character, U'k', false, &context);
    Type(session, U"isha");
    CompleteLatest(session, host, U"記者");
    Press(session, Key::Enter);
    EXPECT_TRUE(host.conversions.empty()); // 取り消せる間は書かない
    Press(session, Key::Reset);
    EXPECT_TRUE(host.conversions.size() == 1);
    if (host.conversions.size() == 1) {
        const Entry& entry = host.conversions[0];
        EXPECT_TRUE(entry.mode == Entry::Mode::Live && entry.contextSource == Entry::ContextSource::Document);
        EXPECT_EQ(entry.context, U"駅で");
        EXPECT_EQ(entry.reading, U"きしゃ");
        EXPECT_TRUE(entry.proposed == std::optional<std::u32string>(U"記者") && entry.edited == false);
        EXPECT_EQ(entry.committed, U"記者");
        EXPECT_TRUE(!entry.segments);
    }

    // 文節変換で候補を選んで確定
    session.Handle(Key::Character, U'k', false, &context);
    Type(session, U"isha");
    CompleteLatest(session, host, U"記者");
    Press(session, Key::Space);
    Press(session, Key::Space);
    EXPECT_EQ(Press(session, Key::Enter).commit, U"汽車");
    Press(session, Key::Reset);
    EXPECT_TRUE(host.conversions.size() == 2);
    if (host.conversions.size() == 2) {
        const Entry& entry = host.conversions[1];
        EXPECT_TRUE(entry.mode == Entry::Mode::Segments && entry.edited == true);
        EXPECT_TRUE(entry.proposed == std::optional<std::u32string>(U"記者"));
        EXPECT_EQ(entry.committed, U"汽車");
        EXPECT_TRUE(entry.segments && entry.segments->size() == 1 && (*entry.segments)[0].result == U"汽車");
    }

    // 表示形（F7 のカタカナ）で確定: ライブ変換の結果を退けた記録
    session.Handle(Key::Character, U'k', false, &context);
    Type(session, U"isha");
    CompleteLatest(session, host, U"記者");
    Press(session, Key::DisplayKatakana);
    EXPECT_EQ(Press(session, Key::Enter).commit, U"キシャ");
    Press(session, Key::Reset);
    EXPECT_TRUE(host.conversions.size() == 3);
    if (host.conversions.size() == 3) {
        const Entry& entry = host.conversions[2];
        EXPECT_TRUE(entry.mode == Entry::Mode::FunctionKey && entry.edited == true);
        EXPECT_TRUE(entry.proposed == std::optional<std::u32string>(U"記者"));
        EXPECT_EQ(entry.committed, U"キシャ");
    }

    // 左文脈がない確定・読みをそのままの 1 文字は残さない
    const std::u32string empty;
    session.Handle(Key::Character, U'k', false, &empty);
    Type(session, U"isha");
    CompleteLatest(session, host, U"記者");
    Press(session, Key::Enter);
    session.Handle(Key::Character, U'n', false, &context);
    Type(session, U"o");
    CompleteLatest(session, host, U"の");
    Press(session, Key::Enter);
    Press(session, Key::Reset);
    EXPECT_TRUE(host.conversions.size() == 3);

    // 取り消した確定は残さない
    session.Handle(Key::Character, U'k', false, &context);
    Type(session, U"isha");
    CompleteLatest(session, host, U"記者");
    Press(session, Key::Enter);
    Press(session, Key::UndoCommit);
    Press(session, Key::Escape);
    Press(session, Key::Escape);
    Press(session, Key::Reset);
    EXPECT_TRUE(host.conversions.size() == 3);

    // 「直した確定だけ」ではライブ変換をそのまま確定したものを残さない。プライベートモードの間は何も残さない
    settings.conversionLogScope = iroha::ConversionLogScope::Corrections;
    session.SetSettings(settings);
    session.Handle(Key::Character, U'k', false, &context);
    Type(session, U"isha");
    CompleteLatest(session, host, U"記者");
    Press(session, Key::Enter);
    Press(session, Key::Reset);
    EXPECT_TRUE(host.conversions.size() == 3);
    settings.conversionLogScope = iroha::ConversionLogScope::All;
    settings.privateMode = true;
    session.SetSettings(settings);
    session.Handle(Key::Character, U'k', false, &context);
    Type(session, U"isha");
    CompleteLatest(session, host, U"記者");
    Press(session, Key::Space);
    Press(session, Key::Space);
    Press(session, Key::Enter);
    Press(session, Key::Reset);
    EXPECT_TRUE(host.conversions.size() == 3);
}

iroha::SessionSettings TypoSettings() {
    iroha::SessionSettings settings;
    settings.typoCorrection = true;
    return settings;
}

// 頼まれた休止の訂正に結果を渡す（直した読みが無ければ「訂正なし」）
bool CompleteTypo(iroha::InputSession& session, FakeSessionHost& host, const std::u32string& corrected) {
    const auto& request = host.typoRequests.back();
    std::optional<iroha::TypoCorrection> correction;
    if (!corrected.empty()) correction = iroha::TypoCorrection{request.reading, corrected, 5};
    return session.CompleteTypoCorrection(request.generation, request.reading, correction);
}

// 入力の休止で読みを直す。直したら「打った読み → 直した読み」を出し、直後の Backspace で取り消す。
// 取り消した読みは再び直さない（macOS版 scheduleTypoCorrection・undoTypoCorrection）
void TestSessionTypoIdleCorrection() {
    FakeSessionHost host;
    host.typoAvailable = true;
    iroha::InputSession session(&host);
    session.SetSettings(TypoSettings());
    auto output = Type(session, U"kak");
    EXPECT_TRUE(host.typoRequests.empty()); // 未解決のローマ字が残っている・4 文字未満
    output = Type(session, U"uinshimashita");
    EXPECT_TRUE(!host.typoRequests.empty() && output.typoPending);
    EXPECT_EQ(host.typoRequests.back().reading, U"かくいんしました");
    EXPECT_TRUE(host.typoRequests.back().delayMs == 500 && host.typoRequests.back().minimumLength == 4);

    EXPECT_TRUE(CompleteTypo(session, host, U"かくにんしました"));
    output = session.Current();
    EXPECT_EQ(output.PreeditText(), U"かくにんしました");
    EXPECT_TRUE(!output.typoPending && output.typoFeedback.has_value());
    if (output.typoFeedback) {
        EXPECT_EQ(output.typoFeedback->Original(), U"かくいんしました");
        EXPECT_EQ(output.typoFeedback->Corrected(), U"かくにんしました");
    }
    EXPECT_EQ(host.liveRequests.back().reading, U"かくにんしました");
    const size_t requests = host.typoRequests.size();
    EXPECT_TRUE(requests >= 1);

    // 直後の Backspace は訂正の取り消し。取り消した読みは再び直さない
    output = Press(session, Key::Backspace);
    EXPECT_EQ(output.PreeditText(), U"かくいんしました");
    EXPECT_TRUE(!output.typoFeedback && !output.typoPending);
    EXPECT_TRUE(host.typoRequests.size() == requests);
    // 次の Backspace は普通に 1 文字消す
    output = Press(session, Key::Backspace);
    EXPECT_EQ(output.PreeditText(), U"かくいんしまし");

    // 直したあと別のキーを打つと、取り消せなくなる（小窓も閉じる）
    Press(session, Key::Escape);
    Press(session, Key::Escape);
    Type(session, U"kakuinshimashita");
    CompleteTypo(session, host, U"かくにんしました");
    output = Type(session, U"ne");
    EXPECT_TRUE(!output.typoFeedback);
    EXPECT_EQ(host.typoRequests.back().reading, U"かくにんしましたね");
    output = Press(session, Key::Backspace); // 取り消せない（普通に 1 文字消す）
    EXPECT_EQ(output.PreeditText(), U"かくにんしました");
}

// 入力が進んだあとに届いた古い結果・末尾に足すだけの訂正は使わない
void TestSessionTypoIgnoresStaleAndTrailing() {
    FakeSessionHost host;
    host.typoAvailable = true;
    iroha::InputSession session(&host);
    session.SetSettings(TypoSettings());
    Type(session, U"kakuinshi");
    const auto stale = host.typoRequests.back();
    Type(session, U"ta");
    EXPECT_TRUE(!session.CompleteTypoCorrection(stale.generation, stale.reading,
                                                iroha::TypoCorrection{stale.reading, U"かくにんし", 5}));
    EXPECT_EQ(session.Current().PreeditText(), U"かくいんした");
    // 「こえて」→「こえて、」は続きの補完なので捨てる（待ちは解く）
    EXPECT_TRUE(CompleteTypo(session, host, U"かくいんした、"));
    const auto output = session.Current();
    EXPECT_EQ(output.PreeditText(), U"かくいんした");
    EXPECT_TRUE(!output.typoPending && !output.typoFeedback);
    // 設定が OFF・モデルが無ければ頼まない
    FakeSessionHost off;
    iroha::InputSession plain(&off);
    plain.SetSettings(TypoSettings());
    Type(plain, U"kakuinshita");
    EXPECT_TRUE(off.typoRequests.empty());
}

// Space で文節変換したときは、読みを書き換えずに訂正を候補ウィンドウに出す（変換ルールの後ろ）。
// 休止で同じ読みを確かめてあれば、その結果を使い回す。訂正の候補を選んだ確定は学習しない
void TestSessionTypoSegmentCandidates() {
    FakeSessionHost host;
    host.typoAvailable = true;
    host.typoTable[U"かくいんしました"] = U"かくにんしました";
    host.table[U"かくにんしました"] = U"確認しました";
    iroha::InputSession session(&host);
    session.SetSettings(TypoSettings());
    Type(session, U"kakuinshimashita");
    CompleteLatest(session, host, U"かくいんしました");
    auto output = Press(session, Key::Space);
    EXPECT_TRUE(output.candidatesVisible && host.correctTypoCalls == 1);
    EXPECT_TRUE(output.candidates.size() >= 3 && output.candidates[1] == U"確認しました" &&
                output.candidates[2] == U"かくにんしました");
    Press(session, Key::Space);
    EXPECT_EQ(Press(session, Key::Enter).commit, U"確認しました");
    Press(session, Key::Reset);
    EXPECT_TRUE(host.commits.empty());

    // 休止で確かめた結果（訂正なし）を使い回す
    Type(session, U"kakuinshimasita");
    CompleteTypo(session, host, U"");
    CompleteLatest(session, host, U"かくいんしました");
    Press(session, Key::Space);
    EXPECT_TRUE(host.correctTypoCalls == 1);
}

// 差分が文節境界をまたぐ訂正は、文全体を訂正した候補を先頭の文節に出し、選んだら文節ごと置き換える
void TestSessionTypoWholeSentence() {
    FakeSessionHost host;
    host.typoAvailable = true;
    host.typoTable[U"きょうはあめ"] = U"きょうあはめ";
    host.table[U"きょうあはめ"] = U"今日阿波目";
    iroha::InputSession session(&host);
    session.SetSettings(TypoSettings());
    Type(session, U"kyouhaame");
    CompleteLatest(session, host, U"今日は雨");
    auto output = Press(session, Key::Space);
    EXPECT_TRUE(output.preedit.size() == 2);
    EXPECT_TRUE(output.candidates.size() >= 2 && output.candidates[1] == U"今日阿波目");
    output = Press(session, Key::Space); // 文全体の訂正を選ぶと、後ろの文節を隠す
    EXPECT_EQ(output.PreeditText(), U"今日阿波目");
    EXPECT_EQ(Press(session, Key::Enter).commit, U"今日阿波目");
    Press(session, Key::Reset);
    EXPECT_TRUE(host.commits.empty());
}

iroha::SessionSettings AISettings() {
    iroha::SessionSettings settings;
    settings.aiAvailable = true;
    settings.aiPresetNames = {U"英訳", U"敬語", U"要約"};
    settings.aiShortcuts = {0x2000D, 0, 0};
    return settings;
}

// AI変換: 結果待ちの間は「日本語 ⇢ 名前 …」を出し、Esc 以外のキーは飲み込む。
// 英訳（日本語でない結果）はそのまま確定し、問い合わせの返事で 1 回だけ渡す（macOS版 handleAICommit）
void TestSessionAICommitTranslation() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    session.SetSettings(AISettings());
    Type(session, U"kyouha");
    CompleteLatest(session, host, U"今日は");
    auto output = session.Handle(Key::AICommit, 0, false, nullptr);
    EXPECT_TRUE(output.aiPending && host.aiRequests.size() == 1);
    EXPECT_TRUE(output.aiShortcuts == std::vector<uint32_t>({0x2000D, 0, 0}));
    EXPECT_EQ(output.PreeditText(), U"今日は ⇢ 英訳 …");
    EXPECT_EQ(host.aiRequests.back().text, U"今日は");
    output = Type(session, U"a"); // 飲み込む
    EXPECT_EQ(output.PreeditText(), U"今日は ⇢ 英訳 …");
    EXPECT_TRUE(session.UpdateAIPartial(host.aiRequests.back().generation, U"Today "));
    EXPECT_EQ(session.Current().PreeditText(), U"今日は ⇢ Today …");
    EXPECT_TRUE(session.CompleteAI(host.aiRequests.back().generation, std::u32string(U"Today is")));
    output = session.TakePolledOutput();
    EXPECT_EQ(output.commit, U"Today is");
    EXPECT_TRUE(!output.aiPending && !output.Composing());
    EXPECT_TRUE(session.TakePolledOutput().commit.empty()); // 1 回だけ
    Press(session, Key::Reset);
    EXPECT_TRUE(host.commits.empty() && host.conversions.empty());

    // Esc で取り消すと元の未確定文字列に戻る。古い結果は使わない
    Type(session, U"kyouha");
    CompleteLatest(session, host, U"今日は");
    session.Handle(Key::AICommit, 0, false, nullptr);
    output = Press(session, Key::Escape);
    EXPECT_TRUE(!output.aiPending && host.aiCancels >= 1);
    EXPECT_EQ(output.PreeditText(), U"今日は");
    EXPECT_TRUE(!session.CompleteAI(host.aiRequests.back().generation, std::u32string(U"Today")));
    // 失敗したら日本語のまま確定する。フォーカスが移る（CommitNow）ときも日本語のまま
    session.Handle(Key::AICommit, 0, false, nullptr);
    session.CompleteAI(host.aiRequests.back().generation, std::nullopt);
    EXPECT_EQ(session.TakePolledOutput().commit, U"今日は");
    Type(session, U"kyouha");
    CompleteLatest(session, host, U"今日は");
    session.Handle(Key::AICommit, 0, false, nullptr);
    EXPECT_EQ(Press(session, Key::CommitNow).commit, U"今日は");

    // AI が使えなければ普通に確定する
    iroha::SessionSettings off = AISettings();
    off.aiAvailable = false;
    session.SetSettings(off);
    Type(session, U"kyouha");
    CompleteLatest(session, host, U"今日は");
    EXPECT_EQ(session.Handle(Key::AICommit, 0, false, nullptr).commit, U"今日は");
}

// AI変換のキーが Shift+Space（英訳の既定）: 入力中・文節変換中は AI変換。候補ウィンドウを開いていれば前の候補、
// 区切り記号の英字入力中は英字の中のスペースのまま（TIP は Shift+Space を Space として送ってくる）
void TestSessionAICommitShiftSpace() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    iroha::SessionSettings settings = AISettings();
    settings.aiShortcuts = {0x40020, 0, 0};
    settings.delimitedAlphabet = true;
    session.SetSettings(settings);
    Type(session, U"kyouha");
    CompleteLatest(session, host, U"今日は");
    auto output = Press(session, Key::Space, true);
    EXPECT_TRUE(output.aiPending && host.aiRequests.size() == 1 && host.aiRequests.back().preset == 0);
    Press(session, Key::Escape);
    Press(session, Key::Escape);
    Press(session, Key::Reset);

    // 候補ウィンドウを開いているときは前の候補
    Type(session, U"kyouha");
    CompleteLatest(session, host, U"今日は");
    Press(session, Key::Space);
    output = Press(session, Key::Space);
    EXPECT_TRUE(!output.candidates.empty() && output.candidateIndex == 1);
    const uint32_t index = output.candidateIndex;
    output = Press(session, Key::Space, true);
    EXPECT_TRUE(!output.aiPending && host.aiRequests.size() == 1);
    EXPECT_TRUE(output.candidateIndex + 1 == index);
    Press(session, Key::Reset);

    // 区切り記号の英字入力中は英字の中のスペース
    Type(session, U"_ab");
    output = Press(session, Key::Space, true);
    EXPECT_TRUE(!output.aiPending && host.aiRequests.size() == 1);
    EXPECT_EQ(Type(session, U"c_").PreeditText(), U"ab c");

    // Shift なしの Space は今までどおり文節変換
    Press(session, Key::Reset);
    Type(session, U"kyouha");
    CompleteLatest(session, host, U"今日は");
    EXPECT_TRUE(Press(session, Key::Space).segmenting && host.aiRequests.size() == 1);
}

// 日本語の結果（敬語・要約など）は確定せず、文節に区切った未確定文字列にする。学習・変換記録・入力履歴に残さない
void TestSessionAICommitJapanese() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    iroha::SessionSettings settings = AISettings();
    settings.conversionLog = true;
    settings.predictive = true;
    session.SetSettings(settings);
    const std::u32string context = U"明日の";
    session.Handle(Key::Character, U'k', false, &context);
    Type(session, U"aigi");
    CompleteLatest(session, host, U"会議");
    session.Handle(Key::AICommit, 1, false, nullptr);
    EXPECT_TRUE(host.aiRequests.back().preset == 1);
    session.CompleteAI(host.aiRequests.back().generation, std::u32string(U"打ち合わせ"));
    auto output = session.TakePolledOutput();
    EXPECT_TRUE(output.segmenting && output.commit.empty());
    EXPECT_EQ(output.PreeditText(), U"打ち合わせ");
    EXPECT_EQ(Press(session, Key::Enter).commit, U"打ち合わせ");
    Press(session, Key::Reset);
    EXPECT_TRUE(host.commits.empty() && host.conversions.empty() && host.recordedHistory.empty());
}

// 候補ウィンドウを Tab で表に広げる。広げている間の ←→ は隣の列の候補へ移り、閉じると 1 列に戻る
void TestSessionCandidateGrid() {
    FakeSessionHost host;
    std::vector<std::u32string> many;
    for (int i = 0; i < 20; ++i) many.push_back(U"候補" + iroha::Utf8ToUtf32(std::to_string(i)));
    host.candidateTable[U"きしゃ"] = many;
    iroha::InputSession session(&host);
    Type(session, U"kisha");
    CompleteLatest(session, host, U"候補0");
    auto output = Press(session, Key::Space);
    EXPECT_TRUE(output.candidatesVisible && !output.candidatesExpanded);
    output = Press(session, Key::Tab);
    EXPECT_TRUE(output.candidatesVisible && output.candidatesExpanded);
    output = Press(session, Key::Right);
    EXPECT_TRUE(output.candidateIndex == 9 && output.candidatesVisible);
    EXPECT_EQ(output.PreeditText(), output.candidates[9]);
    output = Press(session, Key::Tab); // 1 列に戻すと ←→ は文節の移動に戻る
    EXPECT_TRUE(!output.candidatesExpanded && output.candidateIndex == 9);
    Press(session, Key::Tab);
    output = Press(session, Key::Escape); // 閉じると 1 列に戻る
    EXPECT_TRUE(!output.candidatesVisible && !output.candidatesExpanded);
    output = Press(session, Key::Space);
    EXPECT_TRUE(output.candidatesVisible && !output.candidatesExpanded);
}

// 区切り記号で囲んだ英字入力: 「_」から「_」までを打ったとおりの英字にし、1 回の確定で入れる。
// Shift+Space で英字の中にスペース、「__」で記号そのもの、Backspace で開き直す（macOS版と同じ）
void TestSessionDelimitedAlphabet() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    iroha::SessionSettings settings;
    settings.delimitedAlphabet = true;
    session.SetSettings(settings);
    Type(session, U"kyouha");
    CompleteLatest(session, host, U"今日は");
    auto output = Type(session, U"_iphone");
    EXPECT_EQ(output.PreeditText(), U"今日は_iphone");
    output = Press(session, Key::Space, true);
    output = Type(session, U"15_");
    EXPECT_EQ(output.PreeditText(), U"今日はiphone 15");
    Type(session, U"wo");
    CompleteLatest(session, host, U"を");
    EXPECT_EQ(Press(session, Key::Enter).commit, U"今日はiphone 15を");
    Press(session, Key::Reset);

    // 何も打たずに終わりの記号（「__」）は、記号そのものをかな入力に入れる
    output = Type(session, U"a__");
    iroha::RomajiComposer composer;
    composer.Input(U"a_");
    EXPECT_EQ(output.PreeditText(), composer.Display());
    Press(session, Key::Escape);
    Press(session, Key::Escape);

    // Backspace: 英字を消し、開始の記号だけになったらもう一度でやめる。固定した英字は開き直す
    Type(session, U"a_b");
    output = Press(session, Key::Backspace);
    EXPECT_EQ(output.PreeditText(), U"あ_");
    output = Press(session, Key::Backspace);
    EXPECT_EQ(output.PreeditText(), U"あ");
    Type(session, U"_cd_");
    output = Press(session, Key::Backspace);
    EXPECT_EQ(output.PreeditText(), U"あ_c");
    // 設定が OFF なら「_」は普通の文字
    settings.delimitedAlphabet = false;
    session.SetSettings(settings);
    Press(session, Key::Reset);
    output = Type(session, U"a_");
    EXPECT_EQ(output.PreeditText(), composer.Display());
}

iroha::SessionSettings AIFixSettings() {
    iroha::SessionSettings settings;
    settings.aiFix = true;
    settings.aiAvailable = true;
    settings.aiFixShortcut = 0x30041;
    settings.aiFixShortcutLabel = U"Ctrl+Alt+A";
    return settings;
}

// AIで直す: 休止の判定で疑いがあれば窓を出し、キーで AI に送る。案は文節にして差し替え、直後の Backspace で戻す
void TestSessionAIFix() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    session.SetSettings(AIFixSettings());
    Type(session, U"kisha");
    CompleteLatest(session, host, U"記者");
    EXPECT_TRUE(!host.aiFixChecks.empty() && host.aiFixChecks.back().reading == U"きしゃ");
    auto output = session.Current();
    EXPECT_TRUE(output.aiFixPending && output.aiFixShortcut == 0x30041);
    // 疑いが無ければ窓を出さない
    const auto check = host.aiFixChecks.back();
    EXPECT_TRUE(session.CompleteAIFixCheck(check.generation, check.reading, U"記者", {1.5f}));
    EXPECT_TRUE(session.Current().aiFixNotice.empty() && !session.Current().aiFixPending);

    Type(session, U"ga");
    CompleteLatest(session, host, U"記者が");
    const auto second = host.aiFixChecks.back();
    EXPECT_TRUE(session.CompleteAIFixCheck(second.generation, second.reading, U"記者が", {0.3f, 4.0f, 4.0f}));
    EXPECT_EQ(session.Current().aiFixNotice, U"AIで直す（Ctrl+Alt+A）");
    output = Press(session, Key::AIFix);
    EXPECT_TRUE(output.aiFixPending && output.aiFixNotice == U"AIで確認中…" && host.aiFixRequests.size() == 1);
    EXPECT_TRUE(host.aiFixRequests.back().request.userMessage.find(U"読み: キシャガ") != std::u32string::npos);
    iroha::AIOutcome answer;
    answer.text = U"汽車が";
    EXPECT_TRUE(session.CompleteAIFix(host.aiFixRequests.back().generation, answer));
    output = session.Current();
    EXPECT_TRUE(output.segmenting && output.aiFixFeedback.has_value());
    EXPECT_EQ(output.PreeditText(), U"汽車が");
    if (output.aiFixFeedback) EXPECT_EQ(output.aiFixFeedback->Corrected(), U"汽車が");
    // 直後の Backspace で元の変換に戻す。取り消した読みは再び疑わない
    const size_t checks = host.aiFixChecks.size();
    output = Press(session, Key::Backspace);
    EXPECT_TRUE(!output.segmenting && !output.aiFixFeedback);
    EXPECT_EQ(output.PreeditText(), U"記者が");
    EXPECT_TRUE(host.aiFixChecks.size() == checks);

    // 窓が出ていなくても、キーで今の変換を送れる。答えが同じなら知らせだけ
    Press(session, Key::Reset);
    Type(session, U"kisha");
    CompleteLatest(session, host, U"記者");
    Press(session, Key::AIFix);
    iroha::AIOutcome same;
    same.text = U"説明\n記者";
    session.CompleteAIFix(host.aiFixRequests.back().generation, same);
    EXPECT_EQ(session.Current().aiFixNotice, U"AI: このままでよいようです");
    // 送れないとき（変換が届く前）は理由を出す。失敗したら理由を出す
    Press(session, Key::Reset);
    Type(session, U"kisha");
    EXPECT_EQ(Press(session, Key::AIFix).aiFixNotice, U"変換が終わってから押してください");
    CompleteLatest(session, host, U"記者");
    Press(session, Key::AIFix);
    iroha::AIOutcome failed;
    failed.error = "HTTP 401";
    session.CompleteAIFix(host.aiFixRequests.back().generation, failed);
    EXPECT_EQ(session.Current().aiFixNotice, U"AIに頼めませんでした（HTTP 401）");
    // ほかのキーで窓・知らせを閉じる
    EXPECT_TRUE(Type(session, U"a").aiFixNotice.empty());
}

iroha::InputSession::VoiceEvent VoiceEventOf(iroha::InputSession::VoiceEvent::Kind kind, const std::u32string& text = U"",
                                            std::vector<iroha::ReadingToken> tokens = {}) {
    iroha::InputSession::VoiceEvent event;
    event.kind = kind;
    event.text = text;
    event.tokens = std::move(tokens);
    return event;
}

// 「こえ」モード: 押す → 録音を始めた → 途中結果を未確定文字列に出す → 離す → 読みに戻して入れる（確定はしない）。
// 声で入れた合成は、学習・変換記録・入力履歴に残さない
void TestSessionVoiceInput() {
    using Kind = iroha::InputSession::VoiceEvent::Kind;
    FakeSessionHost host;
    host.table[U"あしたのかいぎは"] = U"明日の会議は";
    iroha::InputSession session(&host);
    iroha::SessionSettings settings;
    settings.voiceInput = true;
    settings.voiceKey = 0xA3;
    settings.voiceToggleShortcut = 0x3004B; // Ctrl+Alt+K
    settings.predictive = true;
    settings.conversionLog = true;
    session.SetSettings(settings);

    const std::u32string document = U"前の行\n会議の件";
    auto output = session.Handle(Key::VoiceStart, 0, false, &document);
    EXPECT_TRUE(output.voicePending && output.voiceKey == 0xA3 && output.preedit.empty() && host.voiceStarts.size() == 1);
    EXPECT_TRUE(output.voiceToggleShortcut == 0x3004B);
    const uint64_t generation = host.voiceStarts.back();
    EXPECT_TRUE(session.HandleVoiceEvent(generation, VoiceEventOf(Kind::Started)));
    EXPECT_EQ(session.Current().voiceNotice, U"● 聞いています");
    // 途中結果は未確定文字列に出す（合成の状態には入れない）
    EXPECT_TRUE(session.HandleVoiceEvent(generation, VoiceEventOf(Kind::Partial, U"明日の 会議")));
    output = session.Current();
    EXPECT_EQ(output.PreeditText(), U"明日の会議");
    EXPECT_TRUE(output.voiceNotice.empty());
    output = session.Handle(Key::VoiceStop, 0, false, nullptr);
    EXPECT_TRUE(output.voicePending && host.voiceStops.size() == 1);
    EXPECT_EQ(output.PreeditText(), U"明日の会議");
    // 結果: 読みの部分はライブ変換に任せ、数字の入った語は表記のまま固定する（その前の読みは、そこで変換して固定する）
    EXPECT_TRUE(session.HandleVoiceEvent(
        generation, VoiceEventOf(Kind::Finished, U"", {{U"明日", U"あした"}, {U"の", U"の"}, {U"会議", U"かいぎ"},
                                                        {U"は", U"は"}, {U"3人", std::nullopt}, {U"で", U"で"}})));
    output = session.Current();
    EXPECT_TRUE(!output.voicePending);
    // 変換が届くまでは、認識結果の表記を見せる（かなに戻さない）
    EXPECT_EQ(output.PreeditText(), U"明日の会議は3人で");
    EXPECT_TRUE(!host.liveRequests.empty() && host.liveRequests.back().reading == U"で" &&
                host.liveRequests.back().context == U"会議の件明日の会議は3人");
    CompleteLatest(session, host, U"で");
    // 音声認識の表記と変換結果を比べた知らせ
    EXPECT_EQ(session.Current().voiceNotice, U"音声認識と同じ");
    output = Press(session, Key::Enter);
    EXPECT_EQ(output.commit, U"明日の会議は3人で");
    Type(session, U"a");
    EXPECT_TRUE(host.recordedHistory.empty() && host.conversions.empty() && host.commits.empty());
    Press(session, Key::Reset);

    // 録音中にほかのキー: ショートカットに使っているので捨てる（キーはいつもどおり処理する）
    session.Handle(Key::VoiceStart, 0, false, nullptr);
    session.HandleVoiceEvent(host.voiceStarts.back(), VoiceEventOf(Kind::Started));
    output = Type(session, U"a");
    EXPECT_TRUE(!output.voicePending && host.voiceCancels == 1);
    EXPECT_EQ(output.PreeditText(), U"あ");
    // 取り消したあとに届いた知らせは使わない
    EXPECT_TRUE(!session.HandleVoiceEvent(host.voiceStarts.back(), VoiceEventOf(Kind::Finished, U"", {{U"い", U"い"}})));
    Press(session, Key::Reset);

    // 短すぎた（変換サーバが捨てた）: 何も入れない。聞き取れなかった・始められなかったときは知らせる
    session.Handle(Key::VoiceStart, 0, false, nullptr);
    session.Handle(Key::VoiceStop, 0, false, nullptr);
    session.HandleVoiceEvent(host.voiceStarts.back(), VoiceEventOf(Kind::Discarded));
    output = session.Current();
    EXPECT_TRUE(!output.voicePending && output.preedit.empty() && output.voiceNotice.empty());
    session.Handle(Key::VoiceStart, 0, false, nullptr);
    session.Handle(Key::VoiceStop, 0, false, nullptr);
    session.HandleVoiceEvent(host.voiceStarts.back(), VoiceEventOf(Kind::Finished));
    EXPECT_EQ(session.Current().voiceNotice, U"こえ: 聞き取れませんでした");
    host.voiceStartError = U"日本語の音声認識エンジンが見つかりません";
    output = session.Handle(Key::VoiceStart, 0, false, nullptr);
    EXPECT_TRUE(!output.voicePending);
    EXPECT_EQ(output.voiceNotice, U"こえ: 日本語の音声認識エンジンが見つかりません");

    // 設定が OFF なら何もしない
    host.voiceStartError.clear();
    settings.voiceInput = false;
    session.SetSettings(settings);
    const size_t starts = host.voiceStarts.size();
    output = session.Handle(Key::VoiceStart, 0, false, nullptr);
    EXPECT_TRUE(!output.voicePending && output.voiceKey == 0 && output.voiceToggleShortcut == 0 &&
                host.voiceStarts.size() == starts);
}

// 入力中に声で入れると、打った読みの続きになる。文節変換中なら、そこまでを確定してから入れる
void TestSessionVoiceAppendsToComposition() {
    using Kind = iroha::InputSession::VoiceEvent::Kind;
    FakeSessionHost host;
    iroha::InputSession session(&host);
    iroha::SessionSettings settings;
    settings.voiceInput = true;
    settings.voiceKey = 0xA3;
    session.SetSettings(settings);
    Type(session, U"kyouha");
    CompleteLatest(session, host, U"今日は");
    session.Handle(Key::VoiceStart, 0, false, nullptr);
    session.HandleVoiceEvent(host.voiceStarts.back(), VoiceEventOf(Kind::Started));
    session.HandleVoiceEvent(host.voiceStarts.back(), VoiceEventOf(Kind::Partial, U"晴れ"));
    EXPECT_EQ(session.Current().PreeditText(), U"今日は晴れ");
    session.Handle(Key::VoiceStop, 0, false, nullptr);
    session.HandleVoiceEvent(host.voiceStarts.back(), VoiceEventOf(Kind::Finished, U"", {{U"晴れ", U"はれ"}}));
    EXPECT_TRUE(!host.liveRequests.empty() && host.liveRequests.back().reading == U"きょうははれ");
    // 打った読みに続けたときは、前の変換結果 + 増えたかな
    EXPECT_EQ(session.Current().PreeditText(), U"今日ははれ");
    // 打った読みに続けたときは比べない
    CompleteLatest(session, host, U"今日は晴れ");
    EXPECT_TRUE(session.Current().voiceNotice.empty());

    Press(session, Key::Space);
    EXPECT_TRUE(session.Current().segmenting);
    session.Handle(Key::VoiceStart, 0, false, nullptr);
    session.HandleVoiceEvent(host.voiceStarts.back(), VoiceEventOf(Kind::Started));
    // 文節変換中は途中結果を小窓に出す
    session.HandleVoiceEvent(host.voiceStarts.back(), VoiceEventOf(Kind::Partial, U"です"));
    EXPECT_EQ(session.Current().voiceNotice, U"● です");
    session.Handle(Key::VoiceStop, 0, false, nullptr);
    session.HandleVoiceEvent(host.voiceStarts.back(), VoiceEventOf(Kind::Finished, U"", {{U"です", U"です"}}));
    const auto output = session.TakePolledOutput();
    EXPECT_EQ(output.commit, U"今日は晴れ");
    EXPECT_TRUE(!output.segmenting);
    EXPECT_EQ(output.PreeditText(), U"です");
}

// 声で入れた直後は認識結果の表記を見せ、変換が届いたら置き換える。音声認識との比べは、数字に置き換えた後の認識結果と行う
void TestSessionVoiceShowsRecognizedSurface() {
    using Kind = iroha::InputSession::VoiceEvent::Kind;
    FakeSessionHost host;
    iroha::InputSession session(&host);
    iroha::SessionSettings settings;
    settings.voiceInput = true;
    session.SetSettings(settings);
    session.Handle(Key::VoiceStart, 0, false, nullptr);
    session.Handle(Key::VoiceStop, 0, false, nullptr);
    session.HandleVoiceEvent(host.voiceStarts.back(),
                             VoiceEventOf(Kind::Finished, U"3人で", {{U"三", U"さん"}, {U"人", U"にん"}, {U"で", U"で"}}));
    EXPECT_EQ(session.Current().PreeditText(), U"三人で");
    EXPECT_TRUE(session.Current().voiceNotice.empty());
    CompleteLatest(session, host, U"3人で");
    EXPECT_EQ(session.Current().PreeditText(), U"3人で");
    EXPECT_EQ(session.Current().voiceNotice, U"音声認識と同じ");
    // 読みが変わったら（続けて打った）、認識結果の表記は使わない
    auto output = Type(session, U"a");
    EXPECT_EQ(output.PreeditText(), U"3人であ");
    // 消したあとは、読みが声で入れたときと同じでも、かなで見せる（打った読みと同じ扱い）
    output = Press(session, Key::Backspace);
    EXPECT_EQ(output.PreeditText(), U"さんにんで");
}

// デバッグ表示の行の文面（macOS版 DeveloperOverlay と同じ）
void TestDebugOverlayLines() {
    namespace d = iroha::debug_overlay;
    EXPECT_EQ(d::FormatDuration(12345), U"12.3ms");
    EXPECT_EQ(d::FormatDuration(123456), U"123ms");
    iroha::InferenceStats stats;
    stats.neuralNetworkMicros = 20000;
    stats.neuralNetworkCalls = 2;
    EXPECT_EQ(d::ConversionLine(d::ConversionKind::Candidates, 30000, stats, 5, 2),
              U"候補ウィンドウ  全体 30.0ms ・ NN 20.0ms（2回） ・ 読み 5文字 ・ 取り消し 2");
    iroha::InferenceStats shortcut;
    shortcut.shortcuts = iroha::InferenceStats::kUserDictionary;
    shortcut.modelLoadMicros = 500000;
    EXPECT_EQ(d::ConversionLine(d::ConversionKind::Live, 1000, shortcut, 3, 0),
              U"ライブ変換  全体 1.0ms ・ NN なし（ユーザ辞書） ・ 読み 3文字 ・ モデル読み込み 500ms（全体に含む）");
    EXPECT_EQ(d::TypoLine(d::TypoTrigger::Idle, 4000, stats, 6, d::TypoOutcome::TrailingInsertion),
              U"打ち間違い（入力の休止）  全体 4.0ms ・ NN 20.0ms ・ 読み 6文字 ・ 末尾に足しただけなので捨てた");
    EXPECT_EQ(d::ContextLine(d::ContextSource::Document, U"あいうえおかきくけこさしすせそたちつてとな", std::nullopt),
              U"左文脈  アプリから 21文字 「…いうえおかきくけこさしすせそたちつてとな」");
    EXPECT_EQ(d::ContextLine(d::ContextSource::Document, U"", std::nullopt), U"左文脈  アプリから 0文字（カーソルが行の頭）");
    EXPECT_EQ(d::ContextLine(d::ContextSource::Unavailable, U"", std::u32string()),
              U"左文脈  アプリから読めない（周辺テキストを返さない） → 代わりの確定文字列もなし（文脈なし）");
    EXPECT_EQ(d::ContextLine(d::ContextSource::Disabled, U"", std::u32string(U"今日")),
              U"左文脈  アプリから読む設定が OFF → 代わりに確定文字列 2文字 「今日」");
}

// デバッグ表示: ON のときだけ行を出し、入力の始めに左文脈、変換のたびに時間の行。Enter で消える
void TestSessionDebugOverlay() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    auto output = Type(session, U"kyou");
    EXPECT_TRUE(output.debugOverlay.empty());
    Press(session, Key::Escape);
    Press(session, Key::Escape);

    iroha::SessionSettings settings;
    settings.developerOverlay = true;
    session.SetSettings(settings);
    const std::u32string context = U"会議は";
    output = session.Handle(Key::Character, U'k', false, &context);
    EXPECT_EQ(output.debugOverlay, U"左文脈  アプリから 3文字 「会議は」");
    Type(session, U"you");
    iroha::InferenceReport report;
    report.totalMicros = 8000;
    report.stats.neuralNetworkMicros = 6000;
    report.stats.neuralNetworkCalls = 1;
    session.NoteCancelledConversion();
    const auto& request = host.liveRequests.back();
    const std::u32string converted = U"今日";
    EXPECT_TRUE(session.CompleteLiveConversion(request.generation, request.reading, &converted, &report));
    EXPECT_EQ(session.Current().debugOverlay,
              U"左文脈  アプリから 3文字 「会議は」\nライブ変換  全体 8.0ms ・ NN 6.0ms（1回） ・ 読み 3文字 ・ 取り消し 1");
    // Space で文節に分ける（同じ読みを変換済みなので変換しない）→ 候補ウィンドウ（測る）
    Press(session, Key::Space);
    output = Press(session, Key::Space);
    EXPECT_TRUE(output.debugOverlay.find(U"候補ウィンドウ  全体 ") != std::u32string::npos);
    EXPECT_TRUE(output.debugOverlay.find(U"NN なし") != std::u32string::npos);  // 偽物のエンジンは NN を通さない
    output = Press(session, Key::Enter);
    EXPECT_TRUE(output.debugOverlay.empty());
    // 次の入力の始めに、左文脈が読めなければ代わりの確定文字列を出す
    output = Type(session, U"a");
    EXPECT_TRUE(output.debugOverlay.rfind(U"左文脈  アプリから読めない（周辺テキストを返さない） → 代わりに確定文字列", 0) == 0);
}

// 言語バーのメニューのライブ変換・予測変換の切り替え: 返事に今の設定が載り、入力中に OFF にすると読みに戻る
void TestSessionMenuToggles() {
    FakeSessionHost host;
    iroha::InputSession session(&host);
    auto output = session.Current();
    EXPECT_TRUE(output.liveConversion && !output.predictive && output.punctuation == U"、。");
    Type(session, U"kyou");
    CompleteLatest(session, host, U"今日");
    EXPECT_EQ(session.Current().PreeditText(), U"今日");
    output = Press(session, Key::ToggleLiveConversion);
    EXPECT_TRUE(!output.liveConversion);
    EXPECT_EQ(output.notice, U"ライブ変換: オフ");
    EXPECT_EQ(output.PreeditText(), U"きょう");
    output = Press(session, Key::ToggleLiveConversion);
    EXPECT_TRUE(output.liveConversion && output.livePending);
    output = Press(session, Key::TogglePredictive);
    EXPECT_TRUE(output.predictive);
    EXPECT_EQ(output.notice, U"予測変換: オン");
    output = Press(session, Key::TogglePunctuation);
    EXPECT_TRUE(output.punctuation == U"，．");
}

void RunSessionTests() {
    TestSessionMenuToggles();
    TestDebugOverlayLines();
    TestSessionDebugOverlay();
    TestSessionLiveConversionReplacesKana();
    TestSessionKeepsPreviousConversionAsPrefix();
    TestSessionUsesEarlierLiveResultAsPrefix();
    TestSessionIgnoresCancelledLiveResult();
    TestSessionKeepsPendingRequestForSameReading();
    TestSessionFallsBackToCommittedContext();
    TestSessionSkipsConversionForSameReading();
    TestSessionEnterCommitsLiveResult();
    TestSessionEnterBeforeConversionCatchesUp();
    TestSessionEnterFlushesPendingRomaji();
    TestSessionBackspaceShowsKana();
    TestSessionEscape();
    TestSessionSpaceEntersSegmentModeAndOpensCandidates();
    TestSessionSpaceConvertsWhenNoLiveResult();
    TestSessionSpaceKeepsKanaWhenConversionFails();
    TestSessionCandidatesAndCommit();
    TestSessionKeepsCurrentResultInCandidates();
    TestSessionSymbolCandidates();
    TestSessionTermAnnotations();
    TestSessionRewriteRulesAndCandidateOnlyWords();
    TestSessionMoveSegment();
    TestSessionResizeSegment();
    TestSessionEscapeFromSegmentMode();
    TestSessionRetypeSegment();
    TestSessionSegmentBackspace();
    TestSessionLeftContext();
    TestSessionCommitNowAndReset();
    TestSessionLiveConversionDisabled();
    TestSessionDisplayConversion();
    TestSessionDisplayConversionForgetsLearning();
    TestSessionAlphabetRun();
    TestSessionTogglePunctuation();
    TestSessionIdleSpace();
    TestSessionUndoCommit();
    TestSessionPrediction();
    TestSessionPrivateMode();
    TestSessionConversionLog();
    TestSessionTypoIdleCorrection();
    TestSessionTypoIgnoresStaleAndTrailing();
    TestSessionTypoSegmentCandidates();
    TestSessionTypoWholeSentence();
    TestSessionAICommitTranslation();
    TestSessionAICommitShiftSpace();
    TestSessionAICommitJapanese();
    TestSessionCandidateGrid();
    TestSessionDelimitedAlphabet();
    TestSessionAIFix();
    TestSessionVoiceInput();
    TestSessionVoiceAppendsToComposition();
    TestSessionVoiceShowsRecognizedSurface();
}

} // namespace
