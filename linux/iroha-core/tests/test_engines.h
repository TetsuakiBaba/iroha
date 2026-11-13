#pragma once
// 異体字（VariantKanjiEngine）と長い読みの分割（ChunkedConversionEngine）のテスト。
// 移植元: macos/Tests/IrohaCoreTests/{VariantKanjiEngineTests,ChunkedConversionEngineTests}.swift
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "iroha/chunked_conversion_engine.h"
#include "iroha/conversion_engine.h"
#include "iroha/kana.h"
#include "iroha/variant_kanji_engine.h"

namespace {

using Strings = std::vector<std::u32string>;

// 決まった候補を返すだけのエンジン
class FixedEngine : public iroha::ConversionEngine {
public:
    explicit FixedEngine(Strings results) : results_(std::move(results)) {}
    bool Convert(const std::u32string&, const std::u32string&, int candidateCount,
                 Strings* results, std::string*) override {
        const size_t count = std::min<size_t>(results_.size(), std::max(candidateCount, 1));
        results->assign(results_.begin(), results_.begin() + count);
        return true;
    }

private:
    Strings results_;
};

// 呼び出しを記録するダミーエンジン。table に無い読みはカタカナにして返す
class TableEngine : public iroha::ConversionEngine {
public:
    bool Convert(const std::u32string& reading, const std::u32string& context, int,
                 Strings* results, std::string*) override {
        readings.push_back(reading);
        contexts.push_back(context);
        const auto it = table.find(reading);
        results->assign(1, it != table.end() ? it->second : iroha::HiraganaToKatakana(reading));
        return true;
    }

    std::map<std::u32string, std::u32string> table;
    Strings readings;
    Strings contexts;
};

Strings ConvertWith(iroha::ConversionEngine& engine, const std::u32string& reading,
                    const std::u32string& context, int candidateCount) {
    Strings results;
    std::string error;
    EXPECT_TRUE(engine.Convert(reading, context, candidateCount, &results, &error));
    return results;
}

// 候補ウィンドウでは異体字を末尾に足し、既にあるものは重複させない
void TestVariantsAppendedWithoutDuplicates() {
    FixedEngine base({U"崎", U"先", U"嵜"});
    iroha::VariantKanjiEngine engine(&base);
    EXPECT_TRUE(ConvertWith(engine, U"さき", U"", 8) == (Strings{U"崎", U"先", U"嵜", U"﨑"}));
}

// ライブ変換（候補数1）には影響しない
void TestVariantsDoNotAffectFirstCandidate() {
    FixedEngine base({U"高"});
    iroha::VariantKanjiEngine engine(&base);
    EXPECT_TRUE(ConvertWith(engine, U"たか", U"", 1) == (Strings{U"高"}));
}

// 読みはカタカナでも照合できる。表にない読みは何も足さない
void TestVariantReadingNormalization() {
    EXPECT_TRUE(iroha::VariantKanjiEngine::Variants(U"タカ") == (Strings{U"髙"}));
    EXPECT_TRUE(iroha::VariantKanjiEngine::Variants(U"きしゃ").empty());
}

void TestChunkedShortReadingPassesThrough() {
    TableEngine stub;
    iroha::ChunkedConversionEngine engine(&stub, 10);
    EXPECT_TRUE(ConvertWith(engine, U"きょうはいいてんき", U"文脈", 3) ==
                (Strings{U"キョウハイイテンキ"}));
    EXPECT_TRUE(stub.readings.size() == 1);
    EXPECT_TRUE(stub.contexts == (Strings{U"文脈"}));
}

// 句読点があればその直後で切り、前の区切りの変換結果を次の文脈にする
void TestChunkedSplitsAfterPunctuation() {
    TableEngine stub;
    iroha::ChunkedConversionEngine engine(&stub, 10);
    // 窓「あいうえお、かきくけ」の中の最後の句読点「、」の直後（6文字）で切る
    const auto result = ConvertWith(engine, U"あいうえお、かきくけこさしすせそ", U"前", 1);
    EXPECT_TRUE(stub.readings == (Strings{U"あいうえお、", U"かきくけこさしすせそ"}));
    EXPECT_TRUE(stub.contexts == (Strings{U"前", U"前アイウエオ、"}));
    EXPECT_TRUE(result == (Strings{U"アイウエオ、カキクケコサシスセソ"}));
}

// 句読点がなければ窓を変換して文節境界で切る。最後の文節は次の区切りへ回す
void TestChunkedSplitsAtAlignedSegmentBoundary() {
    TableEngine stub;
    // 窓1「きょうはてんきがいいので」(12文字) → 文節は [今日は][天気がいいので]。最後の文節を除いた4文字で切る
    // 窓2「てんきがいいのでさんぽに」 → [天気がいいので][散歩に] → 8文字で切る
    stub.table[U"きょうはてんきがいいので"] = U"今日は天気がいいので";
    stub.table[U"てんきがいいのでさんぽに"] = U"天気がいいので散歩に";
    iroha::ChunkedConversionEngine engine(&stub, 12);
    const auto result = ConvertWith(engine, U"きょうはてんきがいいのでさんぽにいく", U"", 1);
    EXPECT_TRUE(stub.readings == (Strings{U"きょうはてんきがいいので", U"てんきがいいのでさんぽに",
                                          U"さんぽにいく"}));
    // 先頭側の区切りの変換結果が次の文脈になる
    EXPECT_TRUE(stub.contexts == (Strings{U"", U"今日は", U"今日は天気がいいので"}));
    EXPECT_TRUE(result == (Strings{U"今日は天気がいいのでサンポニイク"}));
}

// 文節境界が取れなければ窓の長さで切る（変換は1回で済ませる）
void TestChunkedFallsBackToWindowLength() {
    TableEngine stub;
    iroha::ChunkedConversionEngine engine(&stub, 5);
    // 5文字の窓はカタカナのみ（=全部1文節）で返るので境界が取れない
    const auto result = ConvertWith(engine, U"あいうえおかきくけこさ", U"", 1);
    EXPECT_TRUE(stub.readings == (Strings{U"あいうえお", U"かきくけこ", U"さ"}));
    EXPECT_TRUE(result == (Strings{U"アイウエオカキクケコサ"}));
}

// ライブ変換で末尾に1文字ずつ足していくとき、先頭側の区切りは再変換しない
void TestChunkedCachesLeadingChunks() {
    TableEngine stub;
    iroha::ChunkedConversionEngine engine(&stub, 10);
    ConvertWith(engine, U"あいうえお、かきくけこさし", U"", 1);
    const size_t before = stub.readings.size();
    EXPECT_TRUE(stub.readings == (Strings{U"あいうえお、", U"かきくけこさし"}));

    ConvertWith(engine, U"あいうえお、かきくけこさしす", U"", 1);
    EXPECT_TRUE(stub.readings.size() == before + 1); // 再変換されるのは末尾の区切りだけ
    EXPECT_EQ(stub.readings.back(), U"かきくけこさしす");

    // 同じ読みをもう一度: 全部キャッシュから返る
    ConvertWith(engine, U"あいうえお、かきくけこさしす", U"", 1);
    EXPECT_TRUE(stub.readings.size() == before + 1);
}

void TestPunctuationCutRespectsMinimum() {
    using Engine = iroha::ChunkedConversionEngine;
    EXPECT_TRUE(Engine::PunctuationCut(U"あいう、えおかき", 3) == size_t(4));
    EXPECT_TRUE(!Engine::PunctuationCut(U"あ、いうえおかき", 3)); // 先頭付近の句読点では切らない
    EXPECT_TRUE(Engine::PunctuationCut(U"あ、いうえお。かき", 3) == size_t(7)); // 最後の句読点を使う
    EXPECT_TRUE(!Engine::PunctuationCut(U"あいうえお", 1));
}

void RunEngineTests() {
    TestVariantsAppendedWithoutDuplicates();
    TestVariantsDoNotAffectFirstCandidate();
    TestVariantReadingNormalization();

    TestChunkedShortReadingPassesThrough();
    TestChunkedSplitsAfterPunctuation();
    TestChunkedSplitsAtAlignedSegmentBoundary();
    TestChunkedFallsBackToWindowLength();
    TestChunkedCachesLeadingChunks();
    TestPunctuationCutRespectsMinimum();
}

} // namespace
