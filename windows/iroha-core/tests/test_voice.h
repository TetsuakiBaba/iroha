#pragma once
// 「こえ」モードの規則のテスト。
// 移植元: macos/Tests/IrohaCoreTests/VoiceTranscriptTests.swift・VoiceComparisonTests.swift（テストケースは 1 対 1）。
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include <string>
#include <vector>

#include "iroha/voice_transcript.h"

namespace {

iroha::ReadingToken VoiceToken(const std::u32string& surface) { return {surface, std::nullopt}; }
iroha::ReadingToken VoiceToken(const std::u32string& surface, const std::u32string& reading) {
    return {surface, reading};
}

// 比べやすい形にする: 読みは「[読み|表記]」、固定部分は「{表記}」
std::u32string DescribeParts(const std::vector<iroha::VoiceTranscript::Part>& parts) {
    std::u32string text;
    for (const auto& part : parts) {
        if (part.kind == iroha::VoiceTranscript::Part::Kind::Reading) {
            text += U"[" + part.text + U"|" + part.surface + U"]";
        } else {
            text += U"{" + part.text + U"}";
        }
    }
    return text;
}

std::u32string DescribeDifferences(const std::vector<iroha::VoiceComparison::Difference>& differences) {
    std::u32string text;
    for (const auto& difference : differences) text += U"(" + difference.recognized + U"→" + difference.converted + U")";
    return text;
}

void TestVoiceReadingsAreJoined() {
    const iroha::VoiceTranscript transcript({
        VoiceToken(U"今日", U"きょう"), VoiceToken(U"は", U"は"), VoiceToken(U"良い", U"よい"),
        VoiceToken(U"天気", U"てんき"), VoiceToken(U"です", U"です"), VoiceToken(U"。", U"。"),
    });
    EXPECT_EQ(DescribeParts(transcript.parts), U"[きょうはよいてんきです。|今日は良い天気です。]");
    EXPECT_EQ(transcript.Surface(), U"今日は良い天気です。");
}

void TestVoiceAlphabetAndDigitsStayFixed() {
    const iroha::VoiceTranscript transcript({
        VoiceToken(U"東京", U"とうきょう"), VoiceToken(U"で", U"で"), VoiceToken(U" "), VoiceToken(U"iPhone"),
        VoiceToken(U"の", U"の"), VoiceToken(U"発表", U"はっぴょう"),
    });
    EXPECT_EQ(DescribeParts(transcript.parts), U"[とうきょうで|東京で]{iPhone}[のはっぴょう|の発表]");
    EXPECT_EQ(transcript.Surface(), U"東京でiPhoneの発表");
    EXPECT_EQ(transcript.ReadingText(), U"とうきょうでiPhoneのはっぴょう");
}

void TestVoiceSpaceBetweenFixedPartsIsKept() {
    const iroha::VoiceTranscript transcript({
        VoiceToken(U"Apple"), VoiceToken(U" "), VoiceToken(U"Watch"), VoiceToken(U" "), VoiceToken(U"を", U"を"),
    });
    EXPECT_EQ(DescribeParts(transcript.parts), U"{Apple Watch}[を|を]");
}

void TestVoiceLeadingSpaceIsDropped() {
    const iroha::VoiceTranscript transcript({VoiceToken(U" "), VoiceToken(U"iPhone"), VoiceToken(U"を", U"を")});
    EXPECT_EQ(DescribeParts(transcript.parts), U"{iPhone}[を|を]");
}

void TestVoiceTypableSymbolsGoIntoReading() {
    const iroha::VoiceTranscript transcript({
        VoiceToken(U"何時", U"なんじ"), VoiceToken(U"から", U"から"), VoiceToken(U"です", U"です"),
        VoiceToken(U"か", U"か"), VoiceToken(U" ？"),
    });
    EXPECT_EQ(DescribeParts(transcript.parts), U"[なんじからですか？|何時からですか？]");
}

void TestVoicePunctuationStyle() {
    const iroha::VoiceTranscript transcript(
        {VoiceToken(U"はい", U"はい"), VoiceToken(U"、", U"、"), VoiceToken(U"そう", U"そう"), VoiceToken(U"。", U"。")},
        U"，", U"．");
    EXPECT_EQ(DescribeParts(transcript.parts), U"[はい，そう．|はい、そう。]");
}

void TestVoiceKanjiWithoutReadingStaysFixed() {
    const iroha::VoiceTranscript transcript({VoiceToken(U"1人"), VoiceToken(U"で", U"で")});
    EXPECT_EQ(DescribeParts(transcript.parts), U"{1人}[で|で]");
}

void TestVoiceCounterAfterDigitsStaysFixed() {
    // 数字と切り離して読みを求めると助数詞の読みを取り違える（「10日」の日 → か）
    const iroha::VoiceTranscript transcript({
        VoiceToken(U"2026"), VoiceToken(U"年", U"ねん"), VoiceToken(U" "), VoiceToken(U"11"), VoiceToken(U"月", U"がつ"),
        VoiceToken(U" "), VoiceToken(U"10"), VoiceToken(U"日", U"か"), VoiceToken(U"立石", U"たていし"),
        VoiceToken(U"さん", U"さん"), VoiceToken(U"と", U"と"), VoiceToken(U" "), VoiceToken(U"3"),
        VoiceToken(U"人", U"にん"), VoiceToken(U"で", U"で"),
    });
    EXPECT_EQ(DescribeParts(transcript.parts), U"{2026年11月10日}[たていしさんと|立石さんと]{3人}[で|で]");
    EXPECT_EQ(transcript.Surface(), U"2026年11月10日立石さんと3人で");
}

void TestVoiceKanaAfterDigitsGoesIntoReading() {
    const iroha::VoiceTranscript transcript({VoiceToken(U"3"), VoiceToken(U"つ", U"つ"), VoiceToken(U"ある", U"ある")});
    EXPECT_EQ(DescribeParts(transcript.parts), U"{3}[つある|つある]");
}

void TestVoiceEmpty() {
    EXPECT_TRUE(iroha::VoiceTranscript(std::vector<iroha::ReadingToken>{}).Empty());
    EXPECT_TRUE(iroha::VoiceTranscript({VoiceToken(U" ")}).Empty());
}

// Windows版だけ: SAPI の表示形・語彙形から読みを決める（2026-11-14 に SAPI の日本語の認識エンジンで見た組）
void TestVoiceTokenFromRecognition() {
    using iroha::VoiceTranscript;
    EXPECT_TRUE(VoiceTranscript::TokenFromRecognition(U"明日", U"あした") == VoiceToken(U"明日", U"あした"));
    // 助詞の「は」は語彙形が「わ」。表示形がかなならそれを使う
    EXPECT_TRUE(VoiceTranscript::TokenFromRecognition(U"は", U"わ") == VoiceToken(U"は", U"は"));
    EXPECT_TRUE(VoiceTranscript::TokenFromRecognition(U"テスト", U"テスト") == VoiceToken(U"テスト", U"てすと"));
    EXPECT_TRUE(VoiceTranscript::TokenFromRecognition(U"東京", U"トウキョウ") == VoiceToken(U"東京", U"とうきょう"));
    EXPECT_TRUE(VoiceTranscript::TokenFromRecognition(U"、", U"、") == VoiceToken(U"、", U"、"));
    EXPECT_TRUE(VoiceTranscript::TokenFromRecognition(U"三", U"さん") == VoiceToken(U"三", U"さん"));
    EXPECT_TRUE(VoiceTranscript::TokenFromRecognition(U"3", U"さん") == VoiceToken(U"3"));
    EXPECT_TRUE(VoiceTranscript::TokenFromRecognition(U"iPhone", U"iPhone") == VoiceToken(U"iPhone"));
    EXPECT_TRUE(VoiceTranscript::TokenFromRecognition(U"3人", U"3人") == VoiceToken(U"3人"));
}

void TestVoiceComparisonSameText() {
    EXPECT_EQ(DescribeDifferences(iroha::VoiceComparison::Differences(U"今日は良い天気です", U"今日は良い天気です")), U"");
    EXPECT_EQ(iroha::VoiceComparison::Summary(U"今日は", U"今日は"), U"音声認識と同じ");
}

void TestVoiceComparisonOneDifference() {
    EXPECT_EQ(DescribeDifferences(
                  iroha::VoiceComparison::Differences(U"明日の会議は何時からですか", U"明日の会議はいつからですか")),
              U"(は何時か→はいつか)");
}

void TestVoiceComparisonAtStartAndEnd() {
    EXPECT_EQ(DescribeDifferences(iroha::VoiceComparison::Differences(U"立石さんと話す", U"停止さんと話す")),
              U"(立石さ→停止さ)");
    EXPECT_EQ(DescribeDifferences(iroha::VoiceComparison::Differences(U"それは機関", U"それは期間")),
              U"(は機関→は期間)");
}

void TestVoiceComparisonSeparate() {
    EXPECT_EQ(DescribeDifferences(iroha::VoiceComparison::Differences(U"立石さんと明日の会議で記譜について話す",
                                                                      U"停止さんと明日の会議で寄付について話す")),
              U"(立石さ→停止さ)(で記譜に→で寄付に)");
}

void TestVoiceComparisonOneCharacterApartAreMerged() {
    // 「行」と「た」の間の同じ文字は「っ」の 1 文字だけ
    EXPECT_EQ(DescribeDifferences(iroha::VoiceComparison::Differences(U"そこに行ったら", U"そこに言ってら")),
              U"(に行ったら→に言ってら)");
}

void TestVoiceComparisonDifferentLengths() {
    EXPECT_EQ(DescribeDifferences(iroha::VoiceComparison::Differences(U"良い天気", U"よい天気")), U"(良い→よい)");
    EXPECT_EQ(DescribeDifferences(iroha::VoiceComparison::Differences(U"今日は", U"今日は。")), U"(は→は。)");
}

void TestVoiceComparisonSummaryLimitsCount() {
    EXPECT_EQ(iroha::VoiceComparison::Summary(U"立石さんと明日の会議で記譜について話す",
                                              U"停止さんと明日の会議で寄付について話す", 1),
              U"音声認識 → iroha: 立石さ → 停止さ　ほか 1 か所");
}

void RunVoiceTests() {
    TestVoiceReadingsAreJoined();
    TestVoiceAlphabetAndDigitsStayFixed();
    TestVoiceSpaceBetweenFixedPartsIsKept();
    TestVoiceLeadingSpaceIsDropped();
    TestVoiceTypableSymbolsGoIntoReading();
    TestVoicePunctuationStyle();
    TestVoiceKanjiWithoutReadingStaysFixed();
    TestVoiceCounterAfterDigitsStaysFixed();
    TestVoiceKanaAfterDigitsGoesIntoReading();
    TestVoiceEmpty();
    TestVoiceTokenFromRecognition();
    TestVoiceComparisonSameText();
    TestVoiceComparisonOneDifference();
    TestVoiceComparisonAtStartAndEnd();
    TestVoiceComparisonSeparate();
    TestVoiceComparisonOneCharacterApartAreMerged();
    TestVoiceComparisonDifferentLengths();
    TestVoiceComparisonSummaryLimitsCount();
}

} // namespace
