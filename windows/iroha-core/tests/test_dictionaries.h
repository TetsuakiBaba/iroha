#pragma once
// ユーザ辞書のテスト（学習のテストは test_learning.h）。
// 移植元: macos/Tests/IrohaCoreTests/UserDictionaryTests.swift
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include <algorithm>
#include <filesystem>
#include <set>
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

std::filesystem::path TempJsonPath(const char* prefix) {
    return std::filesystem::temp_directory_path() /
           (std::string(prefix) + "-" + iroha::GenerateUuid() + ".json");
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

// ---- ライブ変換への適用可否（単語が読みより大幅に長いものは候補ウィンドウ専用） ----

void TestLiveConversionSuitabilityLooksOnlyAtMuchLongerWords() {
    const auto suitable = [](const std::u32string& reading, const std::u32string& word) {
        return iroha::UserDictionary::IsSuitableForLiveConversion(reading, word);
    };
    // 短くなる方向（漢字変換）は常に通す
    EXPECT_TRUE(suitable(U"とうきょうとりつだいがく", U"東京都立大学"));
    EXPECT_TRUE(suitable(U"しょう", U"賞"));
    EXPECT_TRUE(suitable(U"まる", U"○"));
    // 同程度の長さ・軽い略記は通す
    EXPECT_TRUE(suitable(U"あいふぉん", U"iPhone"));
    EXPECT_TRUE(suitable(U"はっしゅたぐ", U"#iroha"));
    EXPECT_TRUE(suitable(U"かぶ", U"株式会社")); // 2倍ちょうど・差2は通す
    // 大幅に長い（2倍超かつ3文字以上）は除外
    EXPECT_TRUE(!suitable(U"たぐ", U"#helloworld #dummytagu"));
    EXPECT_TRUE(!suitable(U"たぐ", U"#hello"));
    EXPECT_TRUE(!suitable(U"めーる", U"someone@example.com"));
    EXPECT_TRUE(!suitable(U"おせわ", U"お世話になっております。"));
}

void TestLiveWordsExcludeCandidateOnlyEntries() {
    const auto dictionary = MakeDict(
        {{U"たぐ", U"#helloworld #dummytagu"}, {U"たぐ", U"タグ"}, {U"きららざか", U"雲母坂"}});
    // 候補ウィンドウ用には全部残る
    EXPECT_TRUE(dictionary.Words(U"たぐ") ==
                (std::vector<std::u32string>{U"#helloworld #dummytagu", U"タグ"}));
    EXPECT_TRUE(dictionary.LiveWords(U"たぐ") == (std::vector<std::u32string>{U"タグ"}));
    EXPECT_TRUE(dictionary.LiveWords(U"きららざか") == (std::vector<std::u32string>{U"雲母坂"}));
    EXPECT_TRUE(!dictionary.IsEmptyForLiveConversion());
}

void TestDictionaryWithOnlyCandidateOnlyEntriesIsEmptyForLiveConversion() {
    const auto dictionary = MakeDict({{U"たぐ", U"#helloworld #dummytagu"}});
    EXPECT_TRUE(!dictionary.IsEmpty());
    EXPECT_TRUE(dictionary.IsEmptyForLiveConversion());
    EXPECT_TRUE(dictionary.LiveWords(U"たぐ").empty());
}

void TestSplitForLiveConversionSkipsCandidateOnlyEntries() {
    using Chunk = iroha::UserDictionary::Chunk;
    const auto dictionary =
        MakeDict({{U"たぐ", U"#helloworld #dummytagu"}, {U"きららざか", U"雲母坂"}});
    EXPECT_TRUE(dictionary.Split(U"たぐをつける") ==
                (std::vector<Chunk>{{true, U"#helloworld #dummytagu"}, {false, U"をつける"}}));
    EXPECT_TRUE(dictionary.Split(U"たぐをつける", 2, true) ==
                (std::vector<Chunk>{{false, U"たぐをつける"}}));
    EXPECT_TRUE(dictionary.Split(U"きららざかのたぐ", 2, true) ==
                (std::vector<Chunk>{{true, U"雲母坂"}, {false, U"のたぐ"}}));
}

void TestCandidateOnlyWordsMatchWholeOrPartialReading() {
    const auto dictionary = MakeDict({{U"たぐ", U"#helloworld #dummytagu"},
                                      {U"きららざか", U"雲母坂"},
                                      {U"め", U"someone@example.com"}});
    using Words = std::vector<std::u32string>;
    EXPECT_TRUE(dictionary.CandidateOnlyWords(U"たぐ") == (Words{U"#helloworld #dummytagu"}));
    EXPECT_TRUE(dictionary.CandidateOnlyWords(U"たぐをつける") == (Words{U"#helloworld #dummytagu"}));
    EXPECT_TRUE(dictionary.CandidateOnlyWords(U"きららざか").empty()); // ライブ変換に使う語は含まない
    EXPECT_TRUE(dictionary.CandidateOnlyWords(U"め") == (Words{U"someone@example.com"})); // 完全一致は1文字でも拾う
    EXPECT_TRUE(dictionary.CandidateOnlyWords(U"めがね").empty()); // 1文字の部分一致は拾わない
    EXPECT_TRUE(iroha::UserDictionary().CandidateOnlyWords(U"たぐ").empty());
}

// ---- 辞書ラティスの語の境目（読みの並びが一致するだけの所には当てない） ----

void TestSplitUsesOnlyMatchesOnWordBoundaries() {
    using Chunk = iroha::UserDictionary::Chunk;
    const auto dictionary = MakeDict({{U"えま", U"江間"}});
    // 「えらーが|きえ|ました」: 「えま」(5..<7) は語の途中なので当てない
    const std::set<int> inside = {0, 3, 4, 6, 9};
    EXPECT_TRUE(dictionary.Split(U"えらーがきえました", 2, false, &inside) ==
                (std::vector<Chunk>{{false, U"えらーがきえました"}}));
    const std::set<int> onBoundary = {0, 2, 3, 6};
    EXPECT_TRUE(dictionary.Split(U"えまとあそぶ", 2, false, &onBoundary) ==
                (std::vector<Chunk>{{true, U"江間"}, {false, U"とあそぶ"}}));
    // 境目が分からなければ（ラティスが無い）読みが並べば当てる
    EXPECT_TRUE(dictionary.Split(U"えらーがきえました") ==
                (std::vector<Chunk>{{false, U"えらーがき"}, {true, U"江間"}, {false, U"した"}}));
}

void TestSplitFallsBackToShorterMatchOnWordBoundaries() {
    using Chunk = iroha::UserDictionary::Chunk;
    const auto dictionary = MakeDict({{U"とうきょう", U"東京"}, {U"とうきょうと", U"東京都"}});
    // 「とうきょう|とっきょ」: 長い一致の終わりが境目でなければ短い一致を使う
    const std::set<int> boundaries = {0, 5, 9};
    EXPECT_TRUE(dictionary.Split(U"とうきょうとっきょ", 2, false, &boundaries) ==
                (std::vector<Chunk>{{true, U"東京"}, {false, U"とっきょ"}}));
}

void TestEntriesOccurringInReading() {
    const auto dictionary = MakeDict({{U"えま", U"江間"}, {U"きららざか", U"雲母坂"}, {U"あ", U"阿"}});
    const auto found = dictionary.EntriesOccurringIn(U"きえました");
    EXPECT_TRUE(found.size() == 1 && found[0].second == U"江間");
    EXPECT_TRUE(dictionary.EntriesOccurringIn(U"あさ").empty()); // 1文字の読みは部分一致の対象外
}

// ---- UserDictionaryEngine（ライブ変換と候補ウィンドウ専用の語） ----

void TestCandidateOnlyWordIsNotUsedForLiveConversion() {
    StubEngine stub;
    const auto dictionary = MakeDict({{U"たぐ", U"#helloworld #dummytagu"}});
    iroha::UserDictionaryEngine engine(&stub, [&dictionary] { return dictionary; });
    // ライブ変換は素通しでエンジンの結果
    EXPECT_TRUE(MustConvert(engine, U"たぐ", U"", 1) == (std::vector<std::u32string>{U"タグ"}));
    EXPECT_TRUE(stub.calls.size() == 1 && stub.calls[0].reading == U"たぐ");
    // 部分一致でも埋め込まない
    EXPECT_TRUE(MustConvert(engine, U"たぐをつける", U"", 1) ==
                (std::vector<std::u32string>{U"タグヲツケル"}));
    // 候補ウィンドウには先頭で出る
    const auto panel = MustConvert(engine, U"たぐ", U"", 3);
    EXPECT_EQ(panel.front(), U"#helloworld #dummytagu");
    EXPECT_TRUE(std::find(panel.begin(), panel.end(), U"タグ") != panel.end());
}

void TestLiveConversionStillUsesShortWordsAlongsideCandidateOnlyOnes() {
    StubEngine stub;
    const auto dictionary = MakeDict(
        {{U"たぐ", U"#helloworld #dummytagu"}, {U"たぐ", U"タグ付け"}, {U"きららざか", U"雲母坂"}});
    iroha::UserDictionaryEngine engine(&stub, [&dictionary] { return dictionary; });
    // 同じ読みでも通常の語はライブ変換に使う
    EXPECT_TRUE(MustConvert(engine, U"たぐ", U"", 1) == (std::vector<std::u32string>{U"タグ付け"}));
    EXPECT_TRUE(MustConvert(engine, U"きららざかのたぐ", U"", 1) ==
                (std::vector<std::u32string>{U"雲母坂ノタグ付け"}));
}

void RunDictionaryTests() {
    TestNormalizedReadingConvertsKatakanaAndTrims();
    TestIsImportableReadingAcceptsKanaOnly();
    TestWordsForReadingKeepsRegistrationOrder();
    TestSplitLeavesReadingIntactWhenNothingMatches();
    TestSplitReplacesMatchedPart();
    TestSplitPrefersLongestMatch();
    TestSplitIgnoresSingleCharacterEntriesInsideSentence();
    TestEmptyDictionaryDoesNotSplit();
    TestLiveConversionSuitabilityLooksOnlyAtMuchLongerWords();
    TestLiveWordsExcludeCandidateOnlyEntries();
    TestDictionaryWithOnlyCandidateOnlyEntriesIsEmptyForLiveConversion();
    TestSplitForLiveConversionSkipsCandidateOnlyEntries();
    TestCandidateOnlyWordsMatchWholeOrPartialReading();
    TestSplitUsesOnlyMatchesOnWordBoundaries();
    TestSplitFallsBackToShorterMatchOnWordBoundaries();
    TestEntriesOccurringInReading();

    TestEmptyDictionaryPassesThrough();
    TestExactMatchSkipsEngine();
    TestPartialMatchComposesWithEngine();
    TestUnmatchedReadingIsConvertedInOneCall();
    TestCandidatePanelPutsUserWordsFirst();
    TestUserWordsSurviveEngineFailure();
    TestEngineFailurePropagatesWithoutUserWords();
    TestCandidateOnlyWordIsNotUsedForLiveConversion();
    TestLiveConversionStillUsesShortWordsAlongsideCandidateOnlyOnes();

    TestAddAndReload();
    TestStoreDropsEmptyEntries();
    TestSyncFromSystemAddsRemovesAndSkips();
    TestSyncKeepsManualEntries();
}

} // namespace
