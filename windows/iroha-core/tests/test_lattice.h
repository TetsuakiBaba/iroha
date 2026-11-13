#pragma once
// 辞書ラティスのテスト。
// 移植元: macos/Tests/IrohaCoreTests/LatticeRescoringEngineTests.swift のラティスだけで確かめられる部分と、
// UserDictionaryTests.swift の testPartialMatchInsideWordIsIgnoredWithLattice（語の境目）。
// 辞書を使わない部品（数の読み・特別な候補・文字の数え方）のテストは Windows 版で足したもの。
// test_main.cpp からインクルードされる（テストマクロを共有するため）。
//
// macOS 版の候補の並びと一致するかは、単体テストではなく
// `iroha-cli lattice-parity <macOS 版の lattice-dump の出力>` で確かめる

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "iroha/lattice_converter.h"
#include "iroha/unicode.h"
#include "lattice/number.h"
#include "lattice/special_candidates.h"

namespace {

bool Contains(const std::vector<std::u32string>& values, const std::u32string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void TestGraphemeCount() {
    EXPECT_TRUE(iroha::GraphemeCount(U"") == 0);
    EXPECT_TRUE(iroha::GraphemeCount(U"きょうは") == 4);
    // 結合濁点・異体字セレクタは前の文字と 1 文字
    EXPECT_TRUE(iroha::GraphemeCount(U"が") == 1);
    EXPECT_TRUE(iroha::GraphemeCount(U"辻\U000E0100") == 1);
    // ZWJ でつないだ絵文字・肌の色・国旗
    EXPECT_TRUE(iroha::GraphemeCount(U"\U0001F468‍\U0001F469‍\U0001F467") == 1);
    EXPECT_TRUE(iroha::GraphemeCount(U"\U0001F44D\U0001F3FD") == 1);
    EXPECT_TRUE(iroha::GraphemeCount(U"\U0001F1EF\U0001F1F5") == 1);
    EXPECT_TRUE(iroha::SplitGraphemes(U"𠮷野家").size() == 3);
}

void TestSpellOutJapanese() {
    using iroha::lattice::SpellOutJapanese;
    EXPECT_EQ(SpellOutJapanese(0), U"〇");
    EXPECT_EQ(SpellOutJapanese(10), U"十");
    EXPECT_EQ(SpellOutJapanese(11), U"十一");
    EXPECT_EQ(SpellOutJapanese(20), U"二十");
    EXPECT_EQ(SpellOutJapanese(100), U"百");
    EXPECT_EQ(SpellOutJapanese(1230), U"千二百三十");
    EXPECT_EQ(SpellOutJapanese(2026), U"二千二十六");
    EXPECT_EQ(SpellOutJapanese(10000), U"一万");
    EXPECT_EQ(SpellOutJapanese(12345), U"一万二千三百四十五");
    EXPECT_EQ(SpellOutJapanese(100000000), U"一億");
    EXPECT_EQ(SpellOutJapanese(-5), U"マイナス五");
}

void TestParseInteger() {
    using iroha::lattice::ParseInteger;
    EXPECT_TRUE(ParseInteger(U"0012") == 12);
    EXPECT_TRUE(ParseInteger(U"-7") == -7);
    EXPECT_TRUE(ParseInteger(U"+7") == 7);
    EXPECT_TRUE(!ParseInteger(U""));
    EXPECT_TRUE(!ParseInteger(U"-"));
    EXPECT_TRUE(!ParseInteger(U"12a"));
    EXPECT_TRUE(!ParseInteger(U"１２")); // 全角は読まない（Swift の Int(_:) と同じ）
    EXPECT_TRUE(ParseInteger(U"9223372036854775807") == INT64_MAX);
    EXPECT_TRUE(!ParseInteger(U"9223372036854775808"));
}

void TestJapaneseNumberDicdata() {
    using iroha::lattice::JapaneseNumberDicdata;
    const auto year = JapaneseNumberDicdata(U"ニセンニジュウロク");
    EXPECT_TRUE(year.size() == 2);
    if (year.size() == 2) {
        EXPECT_EQ(year[0].word, U"二千二十六");
        EXPECT_EQ(year[1].word, U"2026");
    }
    const auto ten = JapaneseNumberDicdata(U"ジュウ");
    EXPECT_TRUE(ten.empty()); // 位だけの読みは数にしない
    const auto digits = JapaneseNumberDicdata(U"イチニサン");
    EXPECT_TRUE(digits.size() == 2);
    if (digits.size() == 2) {
        EXPECT_EQ(digits[0].word, U"一二三");
        EXPECT_EQ(digits[1].word, U"123");
    }
    const auto large = JapaneseNumberDicdata(U"サンマンゴセン");
    EXPECT_TRUE(large.size() == 2);
    if (large.size() == 2) EXPECT_EQ(large[1].word, U"35000");
    EXPECT_TRUE(JapaneseNumberDicdata(U"キョウ").empty());
    EXPECT_TRUE(JapaneseNumberDicdata(U"イチイチジュウ").empty()); // 数字が続いたあとの位
}

std::vector<std::u32string> SpecialWords(const std::u32string& reading) {
    std::vector<std::u32string> words;
    for (const auto& element : iroha::lattice::SpecialCandidates(reading)) {
        words.push_back(element.word);
    }
    return words;
}

void TestSpecialCandidates() {
    EXPECT_TRUE(SpecialWords(U"1230") == (std::vector<std::u32string>{U"1,230", U"12:30"}));
    EXPECT_TRUE(SpecialWords(U"930") == (std::vector<std::u32string>{U"9:30"}));
    EXPECT_TRUE(SpecialWords(U"12345") == (std::vector<std::u32string>{U"12,345"}));
    EXPECT_TRUE(SpecialWords(U"-1234567.89") == (std::vector<std::u32string>{U"-1,234,567.89"}));
    EXPECT_TRUE(SpecialWords(U"1260").size() == 1); // 60 分は時刻にしない
    EXPECT_TRUE(SpecialWords(U"2026ねん") == (std::vector<std::u32string>{U"令和8年"}));
    EXPECT_TRUE(SpecialWords(U"2019ねん") ==
                (std::vector<std::u32string>{U"令和元年", U"平成31年"}));
    // 明治の年（元の実装は「明治-67年」になる。macOS 版の EraYearCandidatesTests と同じ期待値）
    EXPECT_TRUE(SpecialWords(U"1900ねん") == (std::vector<std::u32string>{U"明治33年"}));
    EXPECT_TRUE(SpecialWords(U"1869ねん") == (std::vector<std::u32string>{U"明治2年"}));
    EXPECT_TRUE(SpecialWords(U"1920ねん") == (std::vector<std::u32string>{U"大正9年"}));
    EXPECT_TRUE(SpecialWords(U"めいじ33ねん") == (std::vector<std::u32string>{U"1900年"}));
    EXPECT_TRUE(SpecialWords(U"1800ねん").empty());
    EXPECT_TRUE(SpecialWords(U"れいわ100ねん").empty());
    EXPECT_TRUE(SpecialWords(U"しょうわ64ねん") == (std::vector<std::u32string>{U"1989年"}));
    EXPECT_TRUE(SpecialWords(U"れいわ8ねん") == (std::vector<std::u32string>{U"2026年"}));
    EXPECT_TRUE(SpecialWords(U"へいせいがんねん") == (std::vector<std::u32string>{U"1989年"}));
    EXPECT_TRUE(SpecialWords(U"きょうは").empty());
}

std::unique_ptr<iroha::LatticeConverter> MakeTestLattice() {
    const auto path = iroha::LatticeConverter::DefaultDictionaryPath();
    if (!path) return nullptr;
    return std::make_unique<iroha::LatticeConverter>(*path);
}

// ラティスの候補は読み全体に一致し、読みの合わない語（活用を・NIPPON）を含まない
void TestLatticeCandidatesAllMatchReading(iroha::LatticeConverter& lattice) {
    const auto candidates = lattice.Candidates(U"ないようを", 10);
    EXPECT_TRUE(!candidates.empty());
    EXPECT_TRUE(Contains(candidates, U"内容を"));
    EXPECT_TRUE(!Contains(candidates, U"活用を"));
    EXPECT_TRUE(!Contains(candidates, U"NIPPON"));
    for (const std::u32string& candidate : candidates) {
        EXPECT_TRUE(!candidate.empty() && (candidate.back() == U'を' || candidate.back() == U'ヲ'));
    }
    // 部分候補（「内容」「ない」など）は含まれない
    EXPECT_TRUE(!Contains(candidates, U"内容"));
    EXPECT_TRUE(!Contains(candidates, U"ない"));
    EXPECT_TRUE(candidates.size() <= 10);
}

// 数字・年の読みには、桁区切り・時刻・和暦西暦の候補が入る
void TestSpecialCandidatesForNumbersAndYears(iroha::LatticeConverter& lattice) {
    const auto time = lattice.FullMatchCandidates(U"1230", 10);
    EXPECT_TRUE(Contains(time, U"12:30"));
    EXPECT_TRUE(Contains(time, U"1,230"));
    EXPECT_TRUE(Contains(lattice.FullMatchCandidates(U"12345", 10), U"12,345"));
    EXPECT_TRUE(Contains(lattice.FullMatchCandidates(U"2026ねん", 10), U"令和8年"));
}

// 読みが一致する辞書エントリ（異体字・単漢字）が、文全体の候補の後ろに続く
void TestDictionaryEntriesFollowSentenceCandidates(iroha::LatticeConverter& lattice) {
    const auto kisha = lattice.FullMatchCandidates(U"きしゃ", 10);
    EXPECT_TRUE(Contains(kisha, U"記者"));
    EXPECT_TRUE(Contains(kisha, U"汽車"));
    EXPECT_TRUE(Contains(kisha, U"貴社"));
    const auto saki = lattice.FullMatchCandidates(U"さき", 10);
    EXPECT_TRUE(saki.size() > 10);
    EXPECT_TRUE(Contains(saki, U"先"));
    EXPECT_TRUE(Contains(saki, U"崎"));
    EXPECT_TRUE(Contains(saki, U"﨑"));
    EXPECT_TRUE(saki.size() <= static_cast<size_t>(iroha::LatticeConverter::kMaxFullMatchCandidates));
    std::vector<std::u32string> unique = saki;
    std::sort(unique.begin(), unique.end());
    EXPECT_TRUE(std::adjacent_find(unique.begin(), unique.end()) == unique.end());
}

void TestRawCandidatesAndEmptyReading(iroha::LatticeConverter& lattice) {
    EXPECT_TRUE(lattice.RawCandidates(U"", 10).empty());
    EXPECT_TRUE(lattice.FullMatchCandidates(U"", 10).empty());
    const auto raw = lattice.RawCandidates(U"ないようを", 10);
    bool hasPartial = false;
    for (const auto& candidate : raw) {
        if (candidate.text == U"内容") hasPartial = !candidate.isFullMatch;
    }
    EXPECT_TRUE(hasPartial);
    // 辞書にない文字だけの読みでも、読みそのものの候補は返る
    EXPECT_TRUE(Contains(lattice.FullMatchCandidates(U"ゔゔゔゔ", 5), U"ヴヴヴヴ"));
}

// 読みの並びが一致しても、辞書ラティスで語の境目にならない所はユーザ辞書の語の境目にならない
void TestWordBoundaries(iroha::LatticeConverter& lattice) {
    const std::vector<std::pair<std::u32string, std::u32string>> words = {{U"えま", U"江間"}};
    const auto hasWordAt = [&](const std::u32string& reading, int start) {
        const auto boundaries = lattice.WordBoundaries(reading, words);
        return boundaries && boundaries->count(start) != 0 && boundaries->count(start + 2) != 0;
    };
    // 語として前後とつながる所
    EXPECT_TRUE(hasWordAt(U"えまとあそぶ", 0));
    EXPECT_TRUE(hasWordAt(U"きのうえまにきいた", 3));
    EXPECT_TRUE(hasWordAt(U"えまさん", 0));
    // 動詞などの途中
    EXPECT_TRUE(!hasWordAt(U"えらーがきえました", 5));
    EXPECT_TRUE(!hasWordAt(U"きえます", 1));
    EXPECT_TRUE(!hasWordAt(U"とりかえました", 3));

    const auto boundaries = lattice.WordBoundaries(U"えまとあそぶ", words);
    EXPECT_TRUE(boundaries && boundaries->count(0) != 0 && boundaries->count(6) != 0);
    EXPECT_TRUE(!lattice.WordBoundaries(U"", words));
    // 語はその呼び出しの間だけ入る（候補には混ざらない）
    const std::vector<std::pair<std::u32string, std::u32string>> invented = {
        {U"えま", U"辞書にない江間"}};
    EXPECT_TRUE(lattice.WordBoundaries(U"えまとあそぶ", invented).has_value());
    EXPECT_TRUE(!Contains(lattice.FullMatchCandidates(U"えま", 10), U"辞書にない江間"));
}

void RunLatticeTests() {
    TestGraphemeCount();
    TestSpellOutJapanese();
    TestParseInteger();
    TestJapaneseNumberDicdata();
    TestSpecialCandidates();

    const auto lattice = MakeTestLattice();
    if (!lattice) {
        std::printf("辞書が無いため、辞書ラティスのテストをスキップ"
                    "（windows\\scripts\\fetch-dictionary.ps1 で取得）\n");
        return;
    }
    TestLatticeCandidatesAllMatchReading(*lattice);
    TestSpecialCandidatesForNumbersAndYears(*lattice);
    TestDictionaryEntriesFollowSentenceCandidates(*lattice);
    TestRawCandidatesAndEmptyReading(*lattice);
    TestWordBoundaries(*lattice);
}

} // namespace
