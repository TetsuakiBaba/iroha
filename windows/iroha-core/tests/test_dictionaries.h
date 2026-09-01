#pragma once
// 学習・ユーザ辞書のテスト。
// 移植元: macos/Tests/IrohaCoreTests/{LearningTests,UserDictionaryTests}.swift
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "iroha/conversion_engine.h"
#include "iroha/kana.h"
#include "iroha/unicode.h"
#include "iroha/learning_dictionary.h"
#include "iroha/learning_engine.h"
#include "iroha/learning_store.h"
#include "iroha/user_dictionary.h"
#include "iroha/user_dictionary_engine.h"
#include "iroha/user_dictionary_store.h"

namespace {

// 呼び出しを記録するだけのダミーエンジン（読みをカタカナにして返す）
class StubEngine : public iroha::ConversionEngine {
public:
    struct Call {
        std::u32string reading;
        std::u32string context;
        int count;
    };
    std::vector<Call> calls;
    bool shouldFail = false;

    bool Convert(const std::u32string& reading, const std::u32string& context,
                 int candidateCount, std::vector<std::u32string>* results,
                 std::string* error) override {
        calls.push_back({reading, context, candidateCount});
        if (shouldFail) {
            if (error) *error = "テスト";
            return false;
        }
        results->clear();
        const std::u32string katakana = iroha::HiraganaToKatakana(reading);
        for (int i = 0; i < std::max(1, candidateCount); ++i) {
            results->push_back(i == 0 ? katakana
                                      : katakana + iroha::Utf8ToUtf32(std::to_string(i)));
        }
        return true;
    }
};

std::vector<std::u32string> MustConvert(iroha::ConversionEngine& engine,
                                        const std::u32string& reading,
                                        const std::u32string& context, int count) {
    std::vector<std::u32string> results;
    std::string error;
    if (!engine.Convert(reading, context, count, &results, &error)) {
        return {U"<error>"};
    }
    return results;
}

iroha::LearningEntry Seg(const std::u32string& reading, const std::u32string& result,
                         const std::u32string& context = U"") {
    return {iroha::LearningEntry::Kind::Segment, reading, result, context, 0};
}

std::filesystem::path TempJsonPath(const char* prefix) {
    return std::filesystem::temp_directory_path() /
           (std::string(prefix) + "-" + iroha::GenerateUuid() + ".json");
}

// ---- LearningEngine ----

void TestEmptyLearningPassesThrough() {
    StubEngine stub;
    iroha::LearningEngine engine(&stub, [] { return iroha::LearningDictionary(); });
    EXPECT_EQ(MustConvert(engine, U"きしゃ", U"", 1).front(), U"キシャ");
    EXPECT_TRUE(stub.calls.size() == 1);
}

void TestSentenceMatchSkipsEngine() {
    StubEngine stub;
    const iroha::LearningDictionary dictionary({{iroha::LearningEntry::Kind::Sentence,
                                                 U"きしゃのきしゃ", U"記者の貴社",
                                                 U"", 0}});
    iroha::LearningEngine engine(&stub, [&] { return dictionary; });
    EXPECT_EQ(MustConvert(engine, U"きしゃのきしゃ", U"", 1).front(), U"記者の貴社");
    EXPECT_TRUE(stub.calls.empty()); // 入力全体が一致すればエンジンを呼ばない
}

// 同じ読みでも位置によって違う変換を再現する（この機能の肝）
void TestSegmentsChainWithContext() {
    StubEngine stub;
    const iroha::LearningDictionary dictionary(
        {Seg(U"きしゃの", U"記者の"), Seg(U"きしゃ", U"貴社", U"記者の")});
    iroha::LearningEngine engine(&stub, [&] { return dictionary; });
    EXPECT_EQ(MustConvert(engine, U"きしゃのきしゃがきた", U"", 1).front(),
              U"記者の貴社ガキタ");
    // 学習で埋められなかった「がきた」だけがエンジンに渡る
    EXPECT_TRUE(stub.calls.size() == 1);
    EXPECT_EQ(stub.calls[0].reading, U"がきた");
    EXPECT_EQ(stub.calls[0].context, U"記者の貴社");
}

// 文脈なしで覚えた文節を文中で無差別に当てはめない
void TestContextlessEntryDoesNotFireMidSentence() {
    StubEngine stub;
    const iroha::LearningDictionary dictionary({Seg(U"きしゃ", U"記者")});
    iroha::LearningEngine engine(&stub, [&] { return dictionary; });
    EXPECT_EQ(MustConvert(engine, U"あのきしゃ", U"", 1).front(), U"アノキシャ");
    EXPECT_TRUE(stub.calls.size() == 1); // 読み全体を1回で変換する（分割しない）
    EXPECT_EQ(stub.calls[0].reading, U"あのきしゃ");
}

void TestContextlessEntryFiresAtStart() {
    StubEngine stub;
    const iroha::LearningDictionary dictionary({Seg(U"きしゃ", U"記者")});
    iroha::LearningEngine engine(&stub, [&] { return dictionary; });
    EXPECT_EQ(MustConvert(engine, U"きしゃがきた", U"", 1).front(), U"記者ガキタ");
    EXPECT_TRUE(stub.calls.size() == 1);
    EXPECT_EQ(stub.calls[0].reading, U"がきた");
}

void TestContextMismatchFallsBackToEngine() {
    StubEngine stub;
    const iroha::LearningDictionary dictionary({Seg(U"きしゃ", U"貴社", U"記者の")});
    iroha::LearningEngine engine(&stub, [&] { return dictionary; });
    // 文脈が合わないので学習結果は使わない（読みは分割されるが内容はエンジンの変換）
    EXPECT_EQ(MustConvert(engine, U"あのきしゃ", U"", 1).front(), U"アノキシャ");
    EXPECT_TRUE(stub.calls.size() == 2);
    EXPECT_EQ(stub.calls[0].reading, U"あの");
    EXPECT_EQ(stub.calls[1].reading, U"きしゃ");
}

void TestLongestMatchWins() {
    StubEngine stub;
    const iroha::LearningDictionary dictionary(
        {Seg(U"きしゃ", U"記者"), Seg(U"きしゃに", U"汽車に")});
    iroha::LearningEngine engine(&stub, [&] { return dictionary; });
    EXPECT_EQ(MustConvert(engine, U"きしゃにのる", U"", 1).front(), U"汽車にノル");
}

void TestCandidatePanelPutsLearnedResultsFirst() {
    StubEngine stub;
    const iroha::LearningDictionary dictionary({Seg(U"きしゃ", U"記者")});
    iroha::LearningEngine engine(&stub, [&] { return dictionary; });
    const auto result = MustConvert(engine, U"きしゃ", U"", 3);
    EXPECT_EQ(result.front(), U"記者");
    EXPECT_TRUE(std::find(result.begin(), result.end(), U"キシャ") != result.end());
}

void TestCandidatePanelPrefersContextMatch() {
    StubEngine stub;
    const iroha::LearningDictionary dictionary(
        {Seg(U"きしゃ", U"記者"), Seg(U"きしゃ", U"貴社", U"記者の")});
    iroha::LearningEngine engine(&stub, [&] { return dictionary; });
    const auto result = MustConvert(engine, U"きしゃ", U"きょうは記者の", 3);
    EXPECT_EQ(result.front(), U"貴社"); // 文脈が合う学習結果を優先する
    EXPECT_TRUE(std::find(result.begin(), result.end(), U"記者") != result.end());
}

// ---- LearningStore ----

// ユーザの例:「きしゃのきしゃ」を「記者の貴社」に直したら次から再現される
void TestRecordThenReproduce() {
    const auto path = TempJsonPath("iroha-learning");
    iroha::LearningStore store(path);
    store.Record(U"きしゃのきしゃ", U"記者の貴社",
                 {{U"きしゃの", U"記者の"}, {U"きしゃ", U"貴社"}});

    // 入力全体が同じならそのまま
    const std::u32string* sentence = store.Current().Sentence(U"きしゃのきしゃ");
    EXPECT_TRUE(sentence != nullptr);
    if (sentence) EXPECT_EQ(*sentence, U"記者の貴社");

    // 続きが違っても文節の学習で再現される
    StubEngine stub;
    const iroha::LearningDictionary dictionary = store.Current();
    iroha::LearningEngine engine(&stub, [&] { return dictionary; });
    EXPECT_EQ(MustConvert(engine, U"きしゃのきしゃがきた", U"", 1).front(),
              U"記者の貴社ガキタ");
    std::filesystem::remove(path);
}

void TestLeftContextIsRecordedPerSegment() {
    const auto path = TempJsonPath("iroha-learning");
    iroha::LearningStore store(path);
    store.Record(U"きしゃのきしゃ", U"記者の貴社",
                 {{U"きしゃの", U"記者の"}, {U"きしゃ", U"貴社"}});
    for (const auto& entry : store.Current().Entries()) {
        if (entry.kind != iroha::LearningEntry::Kind::Segment) continue;
        if (entry.result == U"記者の") EXPECT_EQ(entry.leftContext, U"");
        if (entry.result == U"貴社") EXPECT_EQ(entry.leftContext, U"記者の");
    }
    std::filesystem::remove(path);
}

void TestLatestRecordWins() {
    const auto path = TempJsonPath("iroha-learning");
    iroha::LearningStore store(path);
    store.Record(U"きしゃ", U"記者", {{U"きしゃ", U"記者"}});
    store.Record(U"きしゃ", U"汽車", {{U"きしゃ", U"汽車"}});
    const std::u32string* sentence = store.Current().Sentence(U"きしゃ");
    EXPECT_TRUE(sentence != nullptr);
    if (sentence) EXPECT_EQ(*sentence, U"汽車");
    EXPECT_EQ(store.Current().LearnedResults(U"きしゃ", U"").front(), U"汽車");
    std::filesystem::remove(path);
}

void TestPersistsAcrossInstances() {
    const auto path = TempJsonPath("iroha-learning");
    {
        iroha::LearningStore store(path);
        store.Record(U"きしゃのきしゃ", U"記者の貴社",
                     {{U"きしゃの", U"記者の"}, {U"きしゃ", U"貴社"}});
    }
    iroha::LearningStore reloaded(path);
    const std::u32string* sentence = reloaded.Current().Sentence(U"きしゃのきしゃ");
    EXPECT_TRUE(sentence != nullptr);
    if (sentence) EXPECT_EQ(*sentence, U"記者の貴社");
    EXPECT_EQ(reloaded.Current().LearnedResults(U"きしゃ", U"記者の").front(),
              U"貴社");
    std::filesystem::remove(path);
}

void TestResetClearsEverything() {
    const auto path = TempJsonPath("iroha-learning");
    iroha::LearningStore store(path);
    store.Record(U"きしゃ", U"記者", {{U"きしゃ", U"記者"}});
    store.Reset();
    EXPECT_TRUE(store.Current().IsEmpty());
    EXPECT_TRUE(store.Count() == 0);
}

// ---- UserDictionary ----

iroha::UserDictionary MakeDict(
    const std::vector<std::pair<std::u32string, std::u32string>>& pairs) {
    std::vector<iroha::UserDictionaryEntry> entries;
    for (const auto& [reading, word] : pairs) {
        entries.push_back(iroha::MakeUserDictionaryEntry(reading, word));
    }
    return iroha::UserDictionary(std::move(entries));
}

void TestNormalizedReadingConvertsKatakanaAndTrims() {
    EXPECT_EQ(iroha::UserDictionary::NormalizedReading(U" キララザカ "), U"きららざか");
    EXPECT_EQ(iroha::UserDictionary::NormalizedReading(U"きららざか"), U"きららざか");
}

void TestIsImportableReadingAcceptsKanaOnly() {
    EXPECT_TRUE(iroha::UserDictionary::IsImportableReading(U"きららざか"));
    EXPECT_TRUE(iroha::UserDictionary::IsImportableReading(U"キララザカ"));
    EXPECT_TRUE(iroha::UserDictionary::IsImportableReading(U"こーひー"));
    EXPECT_TRUE(!iroha::UserDictionary::IsImportableReading(U"omw"));
    EXPECT_TRUE(!iroha::UserDictionary::IsImportableReading(U"雲母坂"));
    EXPECT_TRUE(!iroha::UserDictionary::IsImportableReading(U""));
}

void TestWordsForReadingKeepsRegistrationOrder() {
    const auto dictionary =
        MakeDict({{U"あきら", U"彰"}, {U"あきら", U"章"}, {U"きららざか", U"雲母坂"}});
    const auto words = dictionary.Words(U"あきら");
    EXPECT_TRUE(words.size() == 2);
    if (words.size() == 2) {
        EXPECT_EQ(words[0], U"彰");
        EXPECT_EQ(words[1], U"章");
    }
    // カタカナで引いてもひらがなに正規化される
    EXPECT_EQ(dictionary.Words(U"キララザカ").front(), U"雲母坂");
    EXPECT_TRUE(dictionary.Words(U"しらない").empty());
}

std::u32string ChunksToString(const std::vector<iroha::UserDictionary::Chunk>& chunks) {
    std::u32string out;
    for (const auto& chunk : chunks) {
        out += chunk.isWord ? U"[W:" : U"[R:";
        out += chunk.text;
        out += U"]";
    }
    return out;
}

void TestSplitLeavesReadingIntactWhenNothingMatches() {
    const auto dictionary = MakeDict({{U"きららざか", U"雲母坂"}});
    EXPECT_EQ(ChunksToString(dictionary.Split(U"きょうはいいてんき")),
              U"[R:きょうはいいてんき]");
}

void TestSplitReplacesMatchedPart() {
    const auto dictionary = MakeDict({{U"きららざか", U"雲母坂"}});
    EXPECT_EQ(ChunksToString(dictionary.Split(U"きららざかにいく")),
              U"[W:雲母坂][R:にいく]");
    EXPECT_EQ(ChunksToString(dictionary.Split(U"あすきららざかへ")),
              U"[R:あす][W:雲母坂][R:へ]");
}

void TestSplitPrefersLongestMatch() {
    const auto dictionary =
        MakeDict({{U"とうきょう", U"東京"}, {U"とうきょうと", U"東京都"}});
    EXPECT_EQ(ChunksToString(dictionary.Split(U"とうきょうとに")),
              U"[W:東京都][R:に]");
}

void TestSplitIgnoresSingleCharacterEntriesInsideSentence() {
    // 1文字の読みが文中で無差別に一致すると変換が壊れるため、部分一致からは外す
    const auto dictionary = MakeDict({{U"あ", U"阿"}});
    EXPECT_EQ(ChunksToString(dictionary.Split(U"あさになった")), U"[R:あさになった]");
    // 完全一致は長さに関係なく拾える
    EXPECT_EQ(dictionary.Words(U"あ").front(), U"阿");
}

void TestEmptyDictionaryDoesNotSplit() {
    const iroha::UserDictionary empty;
    EXPECT_EQ(ChunksToString(empty.Split(U"きららざか")), U"[R:きららざか]");
    EXPECT_TRUE(empty.IsEmpty());
}

// ---- UserDictionaryEngine ----

void TestEmptyDictionaryPassesThrough() {
    StubEngine stub;
    iroha::UserDictionaryEngine engine(&stub, [] { return iroha::UserDictionary(); });
    EXPECT_EQ(MustConvert(engine, U"てすと", U"", 1).front(), U"テスト");
    EXPECT_TRUE(stub.calls.size() == 1);
    EXPECT_EQ(stub.calls[0].reading, U"てすと");
}

void TestExactMatchSkipsEngine() {
    StubEngine stub;
    const auto dictionary = MakeDict({{U"きららざか", U"雲母坂"}});
    iroha::UserDictionaryEngine engine(&stub, [&] { return dictionary; });
    EXPECT_EQ(MustConvert(engine, U"きららざか", U"", 1).front(), U"雲母坂");
    EXPECT_TRUE(stub.calls.empty()); // 完全一致ならエンジンを呼ばない
}

void TestPartialMatchComposesWithEngine() {
    StubEngine stub;
    const auto dictionary = MakeDict({{U"きららざか", U"雲母坂"}});
    iroha::UserDictionaryEngine engine(&stub, [&] { return dictionary; });
    EXPECT_EQ(MustConvert(engine, U"きららざかにいく", U"昨日", 1).front(),
              U"雲母坂ニイク");
    // 残りの読みだけがエンジンに渡り、確定済みの単語が文脈として付く
    EXPECT_TRUE(stub.calls.size() == 1);
    EXPECT_EQ(stub.calls[0].reading, U"にいく");
    EXPECT_EQ(stub.calls[0].context, U"昨日雲母坂");
}

void TestUnmatchedReadingIsConvertedInOneCall() {
    StubEngine stub;
    const auto dictionary = MakeDict({{U"きららざか", U"雲母坂"}});
    iroha::UserDictionaryEngine engine(&stub, [&] { return dictionary; });
    EXPECT_EQ(MustConvert(engine, U"きょうはさむい", U"", 1).front(),
              U"キョウハサムイ");
    EXPECT_TRUE(stub.calls.size() == 1); // 一致しなければ読み全体を1回で変換する
}

void TestCandidatePanelPutsUserWordsFirst() {
    StubEngine stub;
    const auto dictionary =
        MakeDict({{U"きららざか", U"雲母坂"}, {U"きららざか", U"煌坂"}});
    iroha::UserDictionaryEngine engine(&stub, [&] { return dictionary; });
    const auto result = MustConvert(engine, U"きららざか", U"", 3);
    EXPECT_TRUE(result.size() >= 2);
    if (result.size() >= 2) {
        EXPECT_EQ(result[0], U"雲母坂");
        EXPECT_EQ(result[1], U"煌坂");
    }
    EXPECT_TRUE(std::find(result.begin(), result.end(), U"キララザカ") != result.end());
}

void TestUserWordsSurviveEngineFailure() {
    StubEngine stub;
    stub.shouldFail = true;
    const auto dictionary = MakeDict({{U"きららざか", U"雲母坂"}});
    iroha::UserDictionaryEngine engine(&stub, [&] { return dictionary; });
    const auto result = MustConvert(engine, U"きららざか", U"", 5);
    EXPECT_TRUE(result.size() == 1);
    EXPECT_EQ(result.front(), U"雲母坂");
}

void TestEngineFailurePropagatesWithoutUserWords() {
    StubEngine stub;
    stub.shouldFail = true;
    const auto dictionary = MakeDict({{U"きららざか", U"雲母坂"}});
    iroha::UserDictionaryEngine engine(&stub, [&] { return dictionary; });
    std::vector<std::u32string> results;
    std::string error;
    EXPECT_TRUE(!engine.Convert(U"まったくべつ", U"", 5, &results, &error));
}

// ---- UserDictionaryStore ----

void TestAddAndReload() {
    const auto path = TempJsonPath("iroha-dict");
    {
        iroha::UserDictionaryStore store(path);
        store.Add(U"キララザカ", U"雲母坂");
        EXPECT_EQ(store.Current().Words(U"きららざか").front(), U"雲母坂");
    }
    // 別インスタンス（=次回起動）でも読み戻せる
    iroha::UserDictionaryStore reloaded(path);
    EXPECT_EQ(reloaded.Current().Words(U"きららざか").front(), U"雲母坂");
    std::filesystem::remove(path);
}

void TestStoreDropsEmptyEntries() {
    const auto path = TempJsonPath("iroha-dict");
    iroha::UserDictionaryStore store(path);
    store.ReplaceAll({
        iroha::MakeUserDictionaryEntry(U"あ", U""),
        iroha::MakeUserDictionaryEntry(U"", U"空"),
        iroha::MakeUserDictionaryEntry(U"とうきょう", U"東京"),
    });
    EXPECT_TRUE(store.Entries().size() == 1);
    std::filesystem::remove(path);
}

void TestSyncFromSystemAddsRemovesAndSkips() {
    const auto path = TempJsonPath("iroha-dict");
    iroha::UserDictionaryStore store(path);
    auto result = store.SyncFromSystem({
        {U"きららざか", U"雲母坂"},
        {U"とうきょうと", U"東京都"},
        {U"omw", U"On my way!"},
    });
    EXPECT_TRUE(result.added == 2);
    EXPECT_TRUE(result.skipped == 1); // ASCIIショートカットは取り込まない
    EXPECT_TRUE(store.Entries().size() == 2);

    // 2回目は差分だけ: 1件消えて1件増える
    result = store.SyncFromSystem({
        {U"きららざか", U"雲母坂"},
        {U"きょうと", U"京都"},
    });
    EXPECT_TRUE(result.added == 1);
    EXPECT_TRUE(result.removed == 1);
    EXPECT_TRUE(result.unchanged == 1);
    std::u32string words;
    for (const auto& entry : store.Entries()) words += entry.word + U",";
    EXPECT_TRUE(words.find(U"雲母坂") != std::u32string::npos);
    EXPECT_TRUE(words.find(U"京都") != std::u32string::npos);
    EXPECT_TRUE(words.find(U"東京都") == std::u32string::npos);
    std::filesystem::remove(path);
}

void TestSyncKeepsManualEntries() {
    const auto path = TempJsonPath("iroha-dict");
    iroha::UserDictionaryStore store(path);
    store.Add(U"じぶん", U"自分");
    const auto result = store.SyncFromSystem({{U"きららざか", U"雲母坂"}});
    EXPECT_TRUE(result.added == 1);
    EXPECT_TRUE(result.removed == 0);
    // 手動エントリは同期で消えない
    EXPECT_EQ(store.Current().Words(U"じぶん").front(), U"自分");
    std::filesystem::remove(path);
}

void RunDictionaryTests() {
    TestEmptyLearningPassesThrough();
    TestSentenceMatchSkipsEngine();
    TestSegmentsChainWithContext();
    TestContextlessEntryDoesNotFireMidSentence();
    TestContextlessEntryFiresAtStart();
    TestContextMismatchFallsBackToEngine();
    TestLongestMatchWins();
    TestCandidatePanelPutsLearnedResultsFirst();
    TestCandidatePanelPrefersContextMatch();

    TestRecordThenReproduce();
    TestLeftContextIsRecordedPerSegment();
    TestLatestRecordWins();
    TestPersistsAcrossInstances();
    TestResetClearsEverything();

    TestNormalizedReadingConvertsKatakanaAndTrims();
    TestIsImportableReadingAcceptsKanaOnly();
    TestWordsForReadingKeepsRegistrationOrder();
    TestSplitLeavesReadingIntactWhenNothingMatches();
    TestSplitReplacesMatchedPart();
    TestSplitPrefersLongestMatch();
    TestSplitIgnoresSingleCharacterEntriesInsideSentence();
    TestEmptyDictionaryDoesNotSplit();

    TestEmptyDictionaryPassesThrough();
    TestExactMatchSkipsEngine();
    TestPartialMatchComposesWithEngine();
    TestUnmatchedReadingIsConvertedInOneCall();
    TestCandidatePanelPutsUserWordsFirst();
    TestUserWordsSurviveEngineFailure();
    TestEngineFailurePropagatesWithoutUserWords();

    TestAddAndReload();
    TestStoreDropsEmptyEntries();
    TestSyncFromSystemAddsRemovesAndSkips();
    TestSyncKeepsManualEntries();
}

} // namespace
