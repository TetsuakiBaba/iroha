#pragma once
// 選んだ文章の文字数のテスト。
// 移植元: macos/Tests/IrohaCoreTests/CharacterCountTests.swift（テストケースは 1 対 1）。
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include "iroha/character_count.h"

namespace {

void TestCharacterCountGraphemeClusters() {
    const iroha::CharacterCount count(U"こんにちは👨‍👩‍👧");
    EXPECT_TRUE(count.total == 6);
    EXPECT_TRUE(count.withoutWhitespace == 6);
    EXPECT_EQ(count.Summary(), U"6文字");
}

void TestCharacterCountNewlinesAreNotCounted() {
    const iroha::CharacterCount count(U"あい\nうえ\r\nお");
    EXPECT_TRUE(count.total == 5);
    EXPECT_TRUE(count.withoutWhitespace == 5);
}

void TestCharacterCountWhitespaceInTotalOnly() {
    const iroha::CharacterCount count(U"Hello world　テスト\t!");
    EXPECT_TRUE(count.total == 17);
    EXPECT_TRUE(count.withoutWhitespace == 14);
    EXPECT_EQ(count.Summary(), U"17文字（空白除く 14）");
}

void TestCharacterCountEnglishShowsWords() {
    const iroha::CharacterCount count(U"The well-known model, e.g. GPT,\nwas released in 2026.");
    EXPECT_TRUE(count.isEnglish);
    EXPECT_TRUE(count.words == 9);
    EXPECT_TRUE(count.total == 52);
    EXPECT_EQ(count.Summary(), U"9 words (52 chars)");
}

void TestCharacterCountSingularWord() { EXPECT_EQ(iroha::CharacterCount(U"Hello").Summary(), U"1 word (5 chars)"); }

void TestCharacterCountAnyKanaOrKanjiMeansJapanese() {
    // 英語の文に日本語の語が 1 つでも混ざれば文字数の表示
    const iroha::CharacterCount count(U"I visited 東京 last year");
    EXPECT_TRUE(!count.isEnglish);
    EXPECT_EQ(count.Summary(), U"22文字（空白除く 18）");
    EXPECT_TRUE(!iroha::CharacterCount(U"ｱｲｳ abc").isEnglish);
}

void TestCharacterCountDigitsOnlyIsNotEnglish() {
    EXPECT_EQ(iroha::CharacterCount(U"2026 10 03").Summary(), U"10文字（空白除く 8）");
}

void TestCharacterCountEmpty() {
    const iroha::CharacterCount count(U"\n\n");
    EXPECT_TRUE(count.total == 0);
    EXPECT_EQ(count.Summary(), U"0文字");
}

void RunCharacterCountTests() {
    TestCharacterCountGraphemeClusters();
    TestCharacterCountNewlinesAreNotCounted();
    TestCharacterCountWhitespaceInTotalOnly();
    TestCharacterCountEnglishShowsWords();
    TestCharacterCountSingularWord();
    TestCharacterCountAnyKanaOrKanjiMeansJapanese();
    TestCharacterCountDigitsOnlyIsNotEnglish();
    TestCharacterCountEmpty();
}

} // namespace
