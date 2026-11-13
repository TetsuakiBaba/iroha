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

void RunSessionTests() {
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
}

} // namespace
