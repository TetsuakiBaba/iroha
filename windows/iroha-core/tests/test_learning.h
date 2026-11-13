#pragma once
// 学習（LearningEngine / LearningStore）のテスト。
// 移植元: macos/Tests/IrohaCoreTests/LearningTests.swift
// test_main.cpp からインクルードされる（テストマクロと test_dictionaries.h の補助関数を共有するため、
// test_dictionaries.h の後に取り込むこと）。

#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "iroha/kana.h"
#include "iroha/learning_dictionary.h"
#include "iroha/learning_engine.h"
#include "iroha/learning_store.h"

namespace {

// 呼び出しを記録するダミーエンジン。第一候補を読みと文脈から決められる
// （左文脈で変換が変わるエンジンの代わり。既定では読みをカタカナにして返す）
class ContextStub : public iroha::ConversionEngine {
public:
    struct Call {
        std::u32string reading;
        std::u32string context;
        int count;
    };
    std::vector<Call> calls;
    bool shouldFail = false;
    std::function<std::u32string(const std::u32string&, const std::u32string&)> firstCandidate;

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
            if (i == 0) {
                results->push_back(firstCandidate ? firstCandidate(reading, context) : katakana);
            } else {
                results->push_back(katakana + iroha::Utf8ToUtf32(std::to_string(i)));
            }
        }
        return true;
    }
};

iroha::LearningEntry Learned(const std::u32string& reading,
                             const std::optional<std::u32string>& replaced,
                             const std::u32string& result, int64_t updatedAt = 0) {
    iroha::LearningEntry entry;
    entry.reading = reading;
    entry.replaced = replaced;
    entry.result = result;
    entry.updatedAt = updatedAt;
    return entry;
}

bool EndsWith(const std::u32string& text, const std::u32string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// 左文脈で変換が変わるエンジン: 「御恩と」の後だけ「奉公」、それ以外は「方向」
void UseContextEngine(ContextStub* stub) {
    stub->firstCandidate = [](const std::u32string& reading, const std::u32string& context) {
        if (reading != U"ほうこう") return iroha::HiraganaToKatakana(reading);
        return std::u32string(EndsWith(context, U"御恩と") ? U"奉公" : U"方向");
    };
}

iroha::LearningEngine MakeLearningEngine(ContextStub* stub,
                                         std::vector<iroha::LearningEntry> entries) {
    const iroha::LearningDictionary dictionary(std::move(entries));
    return iroha::LearningEngine(stub, [dictionary] { return dictionary; });
}

// ---- LearningEngine ----

void TestEmptyLearningPassesThrough() {
    ContextStub stub;
    auto engine = MakeLearningEngine(&stub, {});
    EXPECT_TRUE(MustConvert(engine, U"きしゃ", U"", 1) == (Strings{U"キシャ"}));
    EXPECT_TRUE(stub.calls.size() == 1);
}

// エンジンが直す前と同じ結果を出したら差し替える
void TestReplacesWhenEngineRepeatsCorrectedResult() {
    ContextStub stub;
    UseContextEngine(&stub);
    auto engine = MakeLearningEngine(&stub, {Learned(U"ほうこう", U"方向", U"芳香")});
    EXPECT_TRUE(MustConvert(engine, U"ほうこう", U"", 1) == (Strings{U"芳香"}));
}

// 左文脈「御恩と」でエンジンが「奉公」を出したら、学習の「芳香」に差し替えない
void TestKeepsEngineResultWhenContextChangesIt() {
    ContextStub stub;
    UseContextEngine(&stub);
    auto engine = MakeLearningEngine(&stub, {Learned(U"ほうこう", U"方向", U"芳香")});
    EXPECT_TRUE(MustConvert(engine, U"ほうこう", U"御恩と", 1) == (Strings{U"奉公"}));
    EXPECT_TRUE(stub.calls.size() == 1 && stub.calls[0].context == U"御恩と");
}

// 差し替えなかったときも、候補ウィンドウでは学習結果を2番目に出す
void TestCandidatePanelPutsLearnedResultSecondWhenNotReplaced() {
    ContextStub stub;
    UseContextEngine(&stub);
    auto engine = MakeLearningEngine(&stub, {Learned(U"ほうこう", U"方向", U"芳香")});
    const auto result = MustConvert(engine, U"ほうこう", U"御恩と", 3);
    EXPECT_TRUE(result.size() >= 2 && result[0] == U"奉公" && result[1] == U"芳香");
}

void TestCandidatePanelPutsLearnedResultFirstWhenReplaced() {
    ContextStub stub;
    UseContextEngine(&stub);
    auto engine = MakeLearningEngine(&stub, {Learned(U"ほうこう", U"方向", U"芳香")});
    const auto result = MustConvert(engine, U"ほうこう", U"", 3);
    EXPECT_EQ(result.front(), U"芳香");
    // エンジンの候補も残る
    EXPECT_TRUE(std::find(result.begin(), result.end(), U"方向") != result.end());
}

// 「直す前」の記録がない学習（この条件を覚える前のもの）は、読みが一致すれば常に差し替える
void TestUnconditionalEntryAlwaysReplaces() {
    ContextStub stub;
    UseContextEngine(&stub);
    auto engine = MakeLearningEngine(&stub, {Learned(U"ほうこう", std::nullopt, U"芳香")});
    EXPECT_TRUE(MustConvert(engine, U"ほうこう", U"御恩と", 1) == (Strings{U"芳香"}));
    // 直す前の結果を記録できるよう、エンジンは呼ぶ
    EXPECT_TRUE(stub.calls.size() == 1);
    EXPECT_TRUE(engine.EngineResult(U"ほうこう", U"芳香") == std::u32string(U"奉公"));
}

// 同じ読みでも「直す前」ごとに別の学習として持てる
void TestEntriesPerEngineResult() {
    ContextStub stub;
    UseContextEngine(&stub);
    auto engine = MakeLearningEngine(&stub, {Learned(U"ほうこう", U"方向", U"芳香"),
                                             Learned(U"ほうこう", U"奉公", U"放校")});
    EXPECT_TRUE(MustConvert(engine, U"ほうこう", U"", 1) == (Strings{U"芳香"}));
    EXPECT_TRUE(MustConvert(engine, U"ほうこう", U"御恩と", 1) == (Strings{U"放校"}));
}

// 直近の第一候補の変換で、学習が差し替える前のエンジンの結果を引ける
void TestEngineResultOfRecentDecision() {
    ContextStub stub;
    UseContextEngine(&stub);
    auto engine = MakeLearningEngine(&stub, {Learned(U"ほうこう", U"方向", U"芳香")});
    MustConvert(engine, U"ほうこう", U"", 1);
    EXPECT_TRUE(engine.EngineResult(U"ほうこう", U"芳香") == std::u32string(U"方向"));
    EXPECT_TRUE(!engine.EngineResult(U"ほうこう", U"奉公")); // 表示と違えば引かない
    EXPECT_TRUE(!engine.EngineResult(U"きしゃ", U"キシャ")); // 学習の無い読みは記録しない
}

// 読み全体が一致しなければ学習は効かない（文中の一部には当てはめない）
void TestPartialReadingDoesNotMatch() {
    ContextStub stub;
    auto engine = MakeLearningEngine(&stub, {Learned(U"きしゃ", std::nullopt, U"貴社")});
    EXPECT_TRUE(MustConvert(engine, U"きしゃがきた", U"", 1) == (Strings{U"キシャガキタ"}));
    // 読みを分割しない
    EXPECT_TRUE(stub.calls.size() == 1 && stub.calls[0].reading == U"きしゃがきた");
}

// 候補ウィンドウはエンジンが失敗しても学習結果だけで開く
void TestCandidatePanelSurvivesEngineFailure() {
    ContextStub stub;
    auto engine = MakeLearningEngine(&stub, {Learned(U"きしゃ", U"キシャ", U"記者")});
    stub.shouldFail = true;
    EXPECT_TRUE(MustConvert(engine, U"きしゃ", U"", 3) == (Strings{U"記者"}));
}

// エンジンが失敗したとき、第一候補で返せるのは条件のない学習だけ
void TestFirstCandidateOnEngineFailure() {
    ContextStub stub1;
    auto conditional = MakeLearningEngine(&stub1, {Learned(U"きしゃ", U"キシャ", U"記者")});
    stub1.shouldFail = true;
    // 直す前と比べられないので差し替えない
    EXPECT_TRUE(MustConvert(conditional, U"きしゃ", U"", 1) == (Strings{U"<error>"}));

    ContextStub stub2;
    auto unconditional = MakeLearningEngine(&stub2, {Learned(U"きしゃ", std::nullopt, U"記者")});
    stub2.shouldFail = true;
    EXPECT_TRUE(MustConvert(unconditional, U"きしゃ", U"", 1) == (Strings{U"記者"}));
}

// 同じ読み・同じ「直す前」のエントリが複数あれば新しい方を使う
void TestNewerEntryWins() {
    ContextStub stub;
    auto engine = MakeLearningEngine(&stub, {Learned(U"きしゃ", U"キシャ", U"記者", 1),
                                             Learned(U"きしゃ", U"キシャ", U"汽車", 2)});
    EXPECT_TRUE(MustConvert(engine, U"きしゃ", U"", 1) == (Strings{U"汽車"}));
}

// ---- LearningStore ----

std::u32string AppliedResult(const iroha::LearningStore& store, const std::u32string& reading,
                             const std::u32string& engineResult) {
    const iroha::LearningEntry* entry = store.Current().Entry(reading, &engineResult);
    return entry ? entry->result : U"<none>";
}

Strings StoredResults(const iroha::LearningStore& store) {
    Strings results;
    for (const iroha::LearningEntry& entry : store.Current().Entries()) {
        results.push_back(entry.result);
    }
    return results;
}

// 「きしゃのきしゃ」を「記者の貴社」に直したら次から再現される
void TestRecordThenReproduce() {
    const auto path = TempJsonPath("iroha-learning");
    iroha::LearningStore store(path);
    store.Record(U"きしゃのきしゃ", U"キシャノキシャ", U"記者の貴社");
    EXPECT_EQ(AppliedResult(store, U"きしゃのきしゃ", U"キシャノキシャ"), U"記者の貴社");

    ContextStub stub;
    iroha::LearningEngine engine(&stub, [&store] { return store.Current(); });
    EXPECT_TRUE(MustConvert(engine, U"きしゃのきしゃ", U"", 1) == (Strings{U"記者の貴社"}));
    std::filesystem::remove(path);
}

// 記録するのは読み全体の1件だけ（文節ごとには覚えない）
void TestRecordStoresSingleEntry() {
    const auto path = TempJsonPath("iroha-learning");
    iroha::LearningStore store(path);
    store.Record(U"きしゃのきしゃ", U"キシャノキシャ", U"記者の貴社");
    EXPECT_TRUE(store.Count() == 1);
    EXPECT_TRUE(store.Current().EntriesForReading(U"きしゃ").empty());
    std::filesystem::remove(path);
}

// 同じ読み・同じ「直す前」なら最新の修正で上書きする
void TestLatestRecordWins() {
    const auto path = TempJsonPath("iroha-learning");
    iroha::LearningStore store(path);
    store.Record(U"きしゃ", U"キシャ", U"記者");
    store.Record(U"きしゃ", U"キシャ", U"汽車");
    EXPECT_TRUE(store.Count() == 1);
    EXPECT_EQ(AppliedResult(store, U"きしゃ", U"キシャ"), U"汽車");
    std::filesystem::remove(path);
}

// 「直す前」が違えば別の学習として残す
void TestDifferentEngineResultsAreSeparateEntries() {
    const auto path = TempJsonPath("iroha-learning");
    iroha::LearningStore store(path);
    store.Record(U"ほうこう", U"方向", U"芳香");
    store.Record(U"ほうこう", U"奉公", U"放校");
    EXPECT_TRUE(store.Count() == 2);
    EXPECT_EQ(AppliedResult(store, U"ほうこう", U"方向"), U"芳香");
    EXPECT_EQ(AppliedResult(store, U"ほうこう", U"奉公"), U"放校");
    EXPECT_EQ(AppliedResult(store, U"ほうこう", U"砲口"), U"<none>");
    std::filesystem::remove(path);
}

// 「直す前」を記録した修正は、同じ読みの記録なしの学習を置き換える
void TestConditionalRecordReplacesUnconditionalEntry() {
    const auto path = TempJsonPath("iroha-learning");
    iroha::LearningStore store(path);
    store.Record(U"ほうこう", std::nullopt, U"芳香");
    store.Record(U"ほうこう", U"奉公", U"放校");
    EXPECT_TRUE(StoredResults(store) == (Strings{U"放校"}));
    EXPECT_EQ(AppliedResult(store, U"ほうこう", U"方向"), U"<none>");
    std::filesystem::remove(path);
}

// 差し替えた結果をエンジンの結果に戻して確定したら、その学習を消す
void TestForgetRemovesAppliedEntry() {
    const auto path = TempJsonPath("iroha-learning");
    iroha::LearningStore store(path);
    store.Record(U"ほうこう", U"方向", U"芳香");
    store.Record(U"ほうこう", U"奉公", U"放校");
    store.Forget(U"ほうこう", U"方向", U"芳香");
    EXPECT_TRUE(StoredResults(store) == (Strings{U"放校"}));
    // 結果が違えば消さない
    store.Forget(U"ほうこう", U"奉公", U"芳香");
    EXPECT_TRUE(store.Count() == 1);
    std::filesystem::remove(path);
}

// 記録なしの古い学習も、戻して確定すれば消える
void TestForgetRemovesUnconditionalEntry() {
    const auto path = TempJsonPath("iroha-learning");
    iroha::LearningStore store(path);
    store.Record(U"ほうこう", std::nullopt, U"芳香");
    store.Forget(U"ほうこう", U"奉公", U"芳香");
    EXPECT_TRUE(store.Current().IsEmpty());
    std::filesystem::remove(path);
}

void TestPersistsAcrossInstances() {
    const auto path = TempJsonPath("iroha-learning");
    {
        iroha::LearningStore store(path);
        store.Record(U"きしゃのきしゃ", U"キシャノキシャ", U"記者の貴社");
    }
    iroha::LearningStore reloaded(path);
    EXPECT_TRUE(reloaded.Count() == 1);
    if (reloaded.Count() == 1) {
        const iroha::LearningEntry& entry = reloaded.Current().Entries().front();
        EXPECT_TRUE(entry.replaced == std::u32string(U"キシャノキシャ"));
        EXPECT_EQ(entry.result, U"記者の貴社");
        EXPECT_TRUE(entry.updatedAt > 0);
    }
    std::filesystem::remove(path);
}

// 文節の学習があった頃のファイルも読める（文節のエントリは捨てる）
void TestLegacySegmentEntriesAreDropped() {
    const auto path = TempJsonPath("iroha-learning");
    {
        std::ofstream file(path, std::ios::binary);
        file << R"({"version":1,"entries":[
          {"kind":"sentence","leftContext":"","reading":"きしゃのきしゃ","result":"記者の貴社",
           "updatedAt":"2026-09-18T00:00:00Z"},
          {"kind":"segment","leftContext":"","reading":"きしゃの","result":"記者の",
           "updatedAt":"2026-09-18T00:00:00Z"},
          {"kind":"segment","leftContext":"記者の","reading":"きしゃ","result":"貴社",
           "updatedAt":"2026-09-18T00:00:00Z"}
        ]})";
    }
    iroha::LearningStore store(path);
    EXPECT_TRUE(StoredResults(store) == (Strings{U"記者の貴社"}));
    // 「直す前」の記録がない学習として読む
    EXPECT_TRUE(store.Count() == 1 && !store.Current().Entries().front().replaced);
    EXPECT_TRUE(store.Current().EntriesForReading(U"きしゃ").empty());
    std::filesystem::remove(path);
}

void TestResetClearsEverything() {
    const auto path = TempJsonPath("iroha-learning");
    iroha::LearningStore store(path);
    store.Record(U"きしゃ", std::nullopt, U"記者");
    store.Reset();
    EXPECT_TRUE(store.Current().IsEmpty());
    EXPECT_TRUE(store.Count() == 0);
}

// 一覧の置き換え（設定画面の編集）は、読み・結果が空のエントリを落とす
void TestReplaceAllDropsEmptyEntries() {
    const auto path = TempJsonPath("iroha-learning");
    iroha::LearningStore store(path);
    store.ReplaceAll({Learned(U"きしゃ", U"キシャ", U"記者", 1), Learned(U"", std::nullopt, U"x", 1),
                      Learned(U"あ", std::nullopt, U"", 1)});
    EXPECT_TRUE(StoredResults(store) == (Strings{U"記者"}));
    std::filesystem::remove(path);
}

void RunLearningTests() {
    TestEmptyLearningPassesThrough();
    TestReplacesWhenEngineRepeatsCorrectedResult();
    TestKeepsEngineResultWhenContextChangesIt();
    TestCandidatePanelPutsLearnedResultSecondWhenNotReplaced();
    TestCandidatePanelPutsLearnedResultFirstWhenReplaced();
    TestUnconditionalEntryAlwaysReplaces();
    TestEntriesPerEngineResult();
    TestEngineResultOfRecentDecision();
    TestPartialReadingDoesNotMatch();
    TestCandidatePanelSurvivesEngineFailure();
    TestFirstCandidateOnEngineFailure();
    TestNewerEntryWins();

    TestRecordThenReproduce();
    TestRecordStoresSingleEntry();
    TestLatestRecordWins();
    TestDifferentEngineResultsAreSeparateEntries();
    TestConditionalRecordReplacesUnconditionalEntry();
    TestForgetRemovesAppliedEntry();
    TestForgetRemovesUnconditionalEntry();
    TestPersistsAcrossInstances();
    TestLegacySegmentEntriesAreDropped();
    TestResetClearsEverything();
    TestReplaceAllDropsEmptyEntries();
}

} // namespace
