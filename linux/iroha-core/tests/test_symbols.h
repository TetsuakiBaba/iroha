#pragma once
// 記号・顔文字・絵文字の候補のテスト。
// 移植元: macos/Tests/IrohaCoreTests/SymbolCandidateTests.swift（テストケースは1対1で対応させること）。
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include <set>
#include <string>
#include <vector>

#include "iroha/symbol_candidates.h"

namespace {

const char kSymbolTsv[] =
    "POS\tCHAR\tReading (space separated)\tdescription\tadditional description\tcategory\tmemo\tunicode\n"
    "記号\t→\tやじるし みぎ ->\t右矢印\t\tSYMBOL\n"
    "記号\t←\tやじるし ひだり\t左矢印\t\tSYMBOL\n"
    "記号\t☺\tえがお かおもじ :)\t顔文字";
const char kEmoticonTsv[] =
    "\tkeys (space separated)\tcategories (space separated)\n"
    "(^^)\tにこにこ にこ\tSMILE\n"
    "(;_;)\tなく\tCRY";
const char kCategorizedTsv[] =
    "# value\tcategory\tdescription\n"
    "(^^)\tSMILE\tにこにこ\n"
    "(๑´ڡ`๑)\tSMILE\tぺろ おいしい";
const char kEmojiTsv[] =
    "# comment\n"
    "1F602\t😂\tわらい なきわらい\t\t絵文字\t\tE0.6\n"
    "1F62D\t😭\tなく\t\t絵文字\t\tE0.6\n"
    "1FAE9\t🫩\tねむい\t\t絵文字\t\tE16.0";

iroha::SymbolCandidateTable MakeSymbolTable(
    const iroha::SymbolCandidateTable::RenderablePredicate& isRenderable = nullptr) {
    return iroha::SymbolCandidateTable(kSymbolTsv, kEmoticonTsv, kCategorizedTsv, kEmojiTsv,
                                       isRenderable);
}

using Strings = std::vector<std::u32string>;

// 読みごとに 3 種類を分けて引ける。かな以外の読み（「->」「:)」）は使わない。カタカナの読みでも引ける
void TestSymbolLookup() {
    const auto table = MakeSymbolTable();
    EXPECT_TRUE(table.Candidates(U"やじるし").symbols == (Strings{U"→", U"←"}));
    iroha::SymbolCandidates crying;
    crying.emoji = {U"😭"};
    crying.emoticons = {U"(;_;)"};
    EXPECT_TRUE(table.Candidates(U"なく") == crying);
    EXPECT_TRUE(table.Candidates(U"ワライ").emoji == (Strings{U"😂"}));
    EXPECT_TRUE(table.Candidates(U"->").Empty());
    EXPECT_TRUE(table.Candidates(U"きしゃ").Empty());
}

// 2 つの顔文字ファイルに同じ顔文字があっても 1 つにまとめる
void TestSymbolEmoticonsAreDeduplicated() {
    const auto table = MakeSymbolTable();
    EXPECT_TRUE(table.Candidates(U"にこにこ").emoticons == (Strings{U"(^^)"}));
    EXPECT_TRUE(table.Candidates(U"かおもじ").emoticons ==
                (Strings{U"(^^)", U"(;_;)", U"(๑´ڡ`๑)"}));
    // 「かおもじ」の読みを持つ記号は記号として出る
    EXPECT_TRUE(table.Candidates(U"かおもじ").symbols == (Strings{U"☺"}));
}

// 「えもじ」で全部の絵文字。描けない絵文字は表に入れない
void TestSymbolAllEmojiAndRenderability() {
    EXPECT_TRUE(MakeSymbolTable().Candidates(U"えもじ").emoji == (Strings{U"😂", U"😭", U"🫩"}));
    const auto table = MakeSymbolTable([](const std::u32string& emoji) { return emoji != U"🫩"; });
    EXPECT_TRUE(table.Candidates(U"えもじ").emoji == (Strings{U"😂", U"😭"}));
    EXPECT_TRUE(table.Candidates(U"ねむい").Empty());
}

const Strings kSymbolEngineCandidates = {U"笑い", U"わらい", U"嗤い", U"ワライ", U"藁位",
                                         U"わら位", U"割らい", U"笑", U"🐱"};

// 絵文字は 7 番目、顔文字・記号はエンジンの候補の後ろ
void TestSymbolEmojiAtSeventhAndOthersAfterEngine() {
    iroha::SymbolCandidates symbols;
    symbols.emoji = {U"😂", U"😁"};
    symbols.emoticons = {U"(^^)"};
    symbols.symbols = {U"☺"};
    const auto merged = iroha::SymbolCandidateMerger::Merge(kSymbolEngineCandidates, symbols, 1);
    EXPECT_TRUE(merged == (Strings{U"笑い", U"わらい", U"嗤い", U"ワライ", U"藁位", U"わら位", U"😂",
                                   U"😁", U"割らい", U"笑", U"🐱", U"(^^)", U"☺"}));
}

// エンジンが出していた絵文字は絵文字の並びへ移す。記号は元の位置のまま
void TestSymbolExistingEmojiMovesAndExistingSymbolStays() {
    iroha::SymbolCandidates symbols;
    symbols.emoji = {U"🐈", U"🐱"};
    symbols.symbols = {U"わらい"};
    const auto merged = iroha::SymbolCandidateMerger::Merge(kSymbolEngineCandidates, symbols, 1);
    EXPECT_TRUE(merged == (Strings{U"笑い", U"わらい", U"嗤い", U"ワライ", U"藁位", U"わら位", U"🐈",
                                   U"🐱", U"割らい", U"笑"}));
}

// 動かさない候補が 7 番目より下まであれば、その後ろに入れる。動かさない範囲にある絵文字はそのまま
void TestSymbolProtectedHeadIsKept() {
    const Strings candidates = {U"😂", U"a", U"b", U"c", U"d", U"e", U"f", U"g", U"h"};
    iroha::SymbolCandidates symbols;
    symbols.emoji = {U"😂", U"😁"};
    const auto merged = iroha::SymbolCandidateMerger::Merge(candidates, symbols, 8);
    EXPECT_TRUE(merged ==
                (Strings{U"😂", U"a", U"b", U"c", U"d", U"e", U"f", U"g", U"😁", U"h"}));
}

// 候補が 7 件に満たなければ末尾に入れる
void TestSymbolShortCandidateList() {
    iroha::SymbolCandidates symbols;
    symbols.emoji = {U"🐱"};
    const auto merged = iroha::SymbolCandidateMerger::Merge({U"猫", U"ねこ"}, symbols, 1);
    EXPECT_TRUE(merged == (Strings{U"猫", U"ねこ", U"🐱"}));
}

// 学習しないのは表に載っていてこの読みで出た候補。漢字・かなだけの候補は普通の語として学習してよい
void TestSymbolUnlearnableCandidates() {
    iroha::SymbolCandidates symbols;
    symbols.emoji = {U"😂"};
    symbols.emoticons = {U"(^^)"};
    symbols.symbols = {U"女", U"♀", U"ゃ"};
    const Strings candidates = {U"女", U"おんな", U"♀", U"😂", U"ゃ"};
    EXPECT_TRUE(iroha::SymbolCandidateMerger::UnlearnableCandidates(candidates, symbols) ==
                (std::set<std::u32string>{U"♀", U"😂"}));
}

// Mozc のデータ（fetch-symbols.ps1 で取得）が読めて、前に足りなかった候補が出る。データが無ければ何もしない
void TestSymbolBundledData() {
    const auto directory = iroha::SymbolCandidateTable::DefaultDirectory({});
    if (!directory) {
        std::printf("skip: 記号のデータが無い（windows/scripts/fetch-symbols.ps1 で取得）\n");
        return;
    }
    const auto table = iroha::SymbolCandidateTable::Load(*directory);
    EXPECT_TRUE(table.has_value());
    if (!table) return;
    const auto contains = [](const Strings& list, const std::u32string& value) {
        return std::find(list.begin(), list.end(), value) != list.end();
    };
    EXPECT_TRUE(contains(table->Candidates(U"わらい").emoji, U"😂"));
    EXPECT_TRUE(contains(table->Candidates(U"ぎりしゃ").symbols, U"α"));
    EXPECT_TRUE(contains(table->Candidates(U"ごめん").emoticons, U"m(_ _)m"));
    EXPECT_TRUE(table->GetCounts().emoji > 1500);
    EXPECT_TRUE(table->GetCounts().emoticons > 400);
}

void RunSymbolTests() {
    TestSymbolLookup();
    TestSymbolEmoticonsAreDeduplicated();
    TestSymbolAllEmojiAndRenderability();
    TestSymbolEmojiAtSeventhAndOthersAfterEngine();
    TestSymbolExistingEmojiMovesAndExistingSymbolStays();
    TestSymbolProtectedHeadIsKept();
    TestSymbolShortCandidateList();
    TestSymbolUnlearnableCandidates();
    TestSymbolBundledData();
}

} // namespace
