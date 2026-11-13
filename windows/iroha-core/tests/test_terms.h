#pragma once
// 専門辞書のテスト。
// 移植元: macos/Tests/IrohaCoreTests/TermDictionaryTests.swift（取得と照合の部分は Windows 専用の
// shared/term_dictionary_fetcher で行うので、ここでは設置の記録・辞書名の印・辞書ラティスを確かめる）。
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "iroha/lattice_converter.h"
#include "iroha/term_dictionary.h"

namespace {

const char kTermSampleText[] =
    "# iroha 専門辞書: テスト\n"
    "# 出典: テスト\n"
    "じょくそう\t褥瘡\n"
    "かんせん\t感染\n"
    "かんせん\t観戦\n"
    "あ\t亜\n"
    "カタカナ\t片仮名\n"
    "よみだけ\n"
    "abc\tエービーシー";

void TestTermParseSkipsCommentsAndInvalidLines() {
    const auto pairs = iroha::TermDictionary::Parse(kTermSampleText);
    std::vector<std::u32string> words;
    for (const auto& pair : pairs) words.push_back(pair.word);
    EXPECT_TRUE(words == (std::vector<std::u32string>{U"褥瘡", U"感染", U"観戦", U"亜", U"片仮名"}));
    // 読みはそのまま（カタカナで書かれていても受け付ける）
    EXPECT_EQ(pairs.back().reading, U"カタカナ");
    // 改行が CRLF でも読める
    EXPECT_TRUE(iroha::TermDictionary::Parse("かんせん\t感染\r\nじょくそう\t褥瘡\r\n").size() == 2);
}

// 読み・語の順（UTF-8 のバイト順）に並べた辞書ファイル
iroha::TermDictionaryLookup::Table TermTable(const std::u32string& label,
                                             std::vector<std::pair<std::u32string, std::u32string>> pairs) {
    std::vector<std::string> lines;
    for (const auto& pair : pairs) {
        lines.push_back(iroha::Utf32ToUtf8(pair.first) + "\t" + iroha::Utf32ToUtf8(pair.second));
    }
    std::sort(lines.begin(), lines.end(), [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(),
            [](char x, char y) { return static_cast<unsigned char>(x) < static_cast<unsigned char>(y); });
    });
    iroha::TermDictionaryLookup::Table table;
    table.label = label;
    table.data = "# 注記\n# 形式: 読み<TAB>語\n";
    for (const std::string& line : lines) table.data += line + "\n";
    table.start = iroha::TermDictionaryLookup::BodyStart(table.data);
    return table;
}

void TestTermLookupFindsDictionaryNames() {
    using Labels = std::vector<std::u32string>;
    auto engineering = TermTable(U"工学", {{U"ちらく", U"地絡"}, {U"ちら", U"散ら"},
                                           {U"へんちょう", U"変調"}, {U"ぞんで", U"ゾンデ"}});
    std::vector<std::pair<std::u32string, std::u32string>> medicinePairs = {
        {U"ぞんで", U"ゾンデ"}, {U"けっしゅ", U"血腫"}, {U"ちらくしょう", U"地絡症"}};
    for (int i = 0; i < 500; ++i) {
        const std::u32string number = iroha::Utf8ToUtf32(std::to_string(i));
        medicinePairs.push_back({U"あ" + number, U"亜" + number});
    }
    auto medicine = TermTable(U"医学", medicinePairs);
    const iroha::TermDictionaryLookup lookup({engineering, medicine});

    EXPECT_TRUE(lookup.Labels(U"ちらく", U"地絡") == (Labels{U"工学"}));
    // 語の後ろに助詞などのかなが続く候補
    EXPECT_TRUE(lookup.Labels(U"ちらくが", U"地絡が") == (Labels{U"工学"}));
    EXPECT_TRUE(lookup.Labels(U"ぞんでを", U"ゾンデを") == (Labels{U"工学", U"医学"}));
    EXPECT_TRUE(lookup.Labels(U"けっしゅ", U"血腫") == (Labels{U"医学"}));
    EXPECT_TRUE(lookup.Labels(U"あ123", U"亜123") == (Labels{U"医学"}));
    // 辞書に無い語・読みの違う語・語の後ろがかなでない候補には付けない
    EXPECT_TRUE(lookup.Labels(U"ちらく", U"散楽").empty());
    EXPECT_TRUE(lookup.Labels(U"へんちょう", U"偏重").empty());
    EXPECT_TRUE(lookup.Labels(U"ちらくが", U"地絡画").empty());
    EXPECT_TRUE(iroha::TermDictionaryLookup().Labels(U"ちらく", U"地絡").empty());
    EXPECT_EQ(iroha::TermDictionaryLookup::Label(U"地名（市区町村・町域）"), U"地名");
    EXPECT_EQ(iroha::TermDictionaryLookup::Label(U"工学"), U"工学");
}

std::string TermCatalogJson(const char* samples) {
    return std::string(
               "{\"formatVersion\":1,\"dictionaries\":[{\"id\":\"test-medicine\",\"name\":\"医学\","
               "\"group\":\"分野別の用語（テスト）\",\"summary\":\"テスト用\",\"entries\":5,"
               "\"kind\":\"noun\",\"file\":{\"url\":\"file:///C:/x.tsv\",\"bytes\":123,"
               "\"sha256\":\"ABCDEF\"},\"license\":\"CC BY-SA 4.0\",\"sources\":[{\"name\":\"JMdict\","
               "\"holder\":\"EDRDG\",\"license\":\"CC BY-SA 4.0\",\"url\":\"https://example.com\","
               "\"licenseURL\":\"https://example.com/licence\"}],\"version\":\"20261108\"") +
           samples + "}]}";
}

// 例の無い前のカタログも読める。新しすぎる形式は読まない
void TestTermCatalog() {
    iroha::TermDictionaryCatalog catalog;
    std::string error;
    EXPECT_TRUE(iroha::TermDictionaryCatalog::Parse(TermCatalogJson(""), &catalog, &error));
    EXPECT_TRUE(catalog.dictionaries.size() == 1 && catalog.dictionaries[0].samples.empty());
    const auto* dictionary = catalog.Find("test-medicine");
    EXPECT_TRUE(dictionary != nullptr);
    if (dictionary) {
        EXPECT_EQ(dictionary->name, U"医学");
        EXPECT_TRUE(dictionary->file.bytes == 123 && dictionary->file.sha256 == "abcdef");
        EXPECT_TRUE(dictionary->sources.size() == 1 &&
                    dictionary->sources[0].licenseUrl == "https://example.com/licence");
    }
    EXPECT_TRUE(iroha::TermDictionaryCatalog::Parse(
        TermCatalogJson(",\"samples\":[{\"word\":\"心筋梗塞\",\"reading\":\"しんきんこうそく\"}]"),
        &catalog, &error));
    EXPECT_TRUE(catalog.dictionaries[0].samples.size() == 1 &&
                catalog.dictionaries[0].samples[0].word == U"心筋梗塞");
    EXPECT_TRUE(!iroha::TermDictionaryCatalog::Parse("{\"formatVersion\":2,\"dictionaries\":[]}",
                                                     &catalog, &error));
}

// 設置の記録は辞書ファイルと揃ったものだけを読み、外すと読まない。記録は macOS版と同じ形で読み書きできる
void TestTermInstallRecords() {
    const std::filesystem::path data =
        std::filesystem::temp_directory_path() / ("iroha-term-" + iroha::GenerateUuid());
    iroha::TermDictionaryCatalog catalog;
    std::string error;
    iroha::TermDictionaryCatalog::Parse(TermCatalogJson(""), &catalog, &error);
    const auto record =
        iroha::TermDictionaryInstall::Record::From(catalog.dictionaries[0], "2026-11-13T10:00:00Z");
    EXPECT_TRUE(iroha::TermDictionaryInstall::WriteRecord(data, record));
    // 辞書ファイルが無ければ入っていない
    EXPECT_TRUE(iroha::TermDictionaryInstall::InstalledRecords(data).empty());
    {
        std::ofstream file(iroha::TermDictionaryInstall::FilePath(data, "test-medicine"), std::ios::binary);
        file << kTermSampleText;
    }
    const auto records = iroha::TermDictionaryInstall::InstalledRecords(data);
    EXPECT_TRUE(records.size() == 1);
    if (records.size() == 1) {
        EXPECT_TRUE(records[0].id == "test-medicine" && records[0].sha256 == "abcdef");
        EXPECT_EQ(records[0].group, U"分野別の用語（テスト）");
        EXPECT_TRUE(records[0].sources.size() == 1 && records[0].sources[0].name == U"JMdict");
        EXPECT_TRUE(!iroha::TermDictionaryInstall::NeedsUpdate(records[0], catalog.dictionaries[0]));
    }
    // macOS版の JSONEncoder（prettyPrinted）が書いた記録も読める
    const auto mac = iroha::TermDictionaryInstall::DecodeRecord(
        "{\n  \"entries\" : 4477,\n  \"group\" : \"分野別の用語（JMdict）\",\n  \"id\" : \"jmdict-medicine\",\n"
        "  \"installedAt\" : \"2026-11-08T05:00:00Z\",\n  \"kind\" : \"noun\",\n  \"license\" : \"CC BY-SA 4.0\",\n"
        "  \"name\" : \"医学\",\n  \"sha256\" : \"cc14\",\n  \"sources\" : [\n\n  ],\n  \"version\" : \"20261108\"\n}");
    EXPECT_TRUE(mac.has_value() && mac->entries == 4477 && mac->name == U"医学");

    EXPECT_TRUE(iroha::TermDictionaryInstall::Remove(data, "test-medicine"));
    EXPECT_TRUE(iroha::TermDictionaryInstall::InstalledRecords(data).empty());
    std::error_code ignored;
    std::filesystem::remove_all(data, ignored);
}

void TestTermIdMustBeSafeForFileNames() {
    EXPECT_TRUE(iroha::TermDictionaryInstall::IsValidId("jmdict-medicine"));
    EXPECT_TRUE(!iroha::TermDictionaryInstall::IsValidId("../escape"));
    EXPECT_TRUE(!iroha::TermDictionaryInstall::IsValidId("Upper"));
    EXPECT_TRUE(!iroha::TermDictionaryInstall::IsValidId(""));
}

// 専門辞書の語は、候補を作るときにラティスに加わる（本体の辞書に無い語・頭の部分も無い語でも候補に出る）
void TestTermLatticeOffersWords() {
    const auto path = iroha::LatticeConverter::DefaultDictionaryPath();
    if (!path) {
        std::printf("skip: 辞書ラティスの辞書が無い（windows/scripts/fetch-dictionary.ps1 で取得）\n");
        return;
    }
    // 1文字の読み（あ → 亜）は入れない
    const auto sample = iroha::LatticeConverter::BuildTermIndex(
        *path, {{iroha::TermKind::kNoun, iroha::TermDictionary::Parse(kTermSampleText)}});
    EXPECT_TRUE(iroha::LatticeConverter::TermIndexEntryCount(*sample) == 4);

    const auto terms = iroha::LatticeConverter::BuildTermIndex(
        *path, {{iroha::TermKind::kNoun, {{U"もげもげざい", U"藻毛藻毛剤"}}},
                {iroha::TermKind::kPerson, {{U"ぬぬぬぬたろう", U"奴奴奴奴太郎"}}}});
    EXPECT_TRUE(iroha::LatticeConverter::TermIndexEntryCount(*terms) == 2);
    iroha::LatticeConverter plain(*path);
    iroha::LatticeConverter withTerms(*path);
    withTerms.SetTermDictionary(terms);

    const auto has = [](const std::vector<std::u32string>& list, const std::u32string& prefix) {
        return std::any_of(list.begin(), list.end(), [&](const std::u32string& candidate) {
            return candidate.compare(0, prefix.size(), prefix) == 0;
        });
    };
    EXPECT_TRUE(!has(plain.FullMatchCandidates(U"もげもげざい", 10), U"藻毛藻毛剤"));
    EXPECT_TRUE(has(withTerms.FullMatchCandidates(U"もげもげざい", 10), U"藻毛藻毛剤"));
    // 助詞が続いても、語として前後とつながる
    EXPECT_TRUE(has(withTerms.FullMatchCandidates(U"もげもげざいをのむ", 10), U"藻毛藻毛剤"));
    EXPECT_TRUE(has(withTerms.FullMatchCandidates(U"ぬぬぬぬたろうさん", 10), U"奴奴奴奴太郎"));
    // ユーザ辞書の語の境目の判定には専門辞書を入れない
    EXPECT_TRUE(withTerms.WordBoundaries(U"もげもげざい", {}) == plain.WordBoundaries(U"もげもげざい", {}));
    // 外せば出ない
    withTerms.SetTermDictionary(nullptr);
    EXPECT_TRUE(!has(withTerms.FullMatchCandidates(U"もげもげざい", 10), U"藻毛藻毛剤"));
}

void RunTermTests() {
    TestTermParseSkipsCommentsAndInvalidLines();
    TestTermLookupFindsDictionaryNames();
    TestTermCatalog();
    TestTermInstallRecords();
    TestTermIdMustBeSafeForFileNames();
    TestTermLatticeOffersWords();
}

} // namespace
