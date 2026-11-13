#pragma once
// 入力履歴のテスト。
// 移植元: macos/Tests/IrohaCoreTests/InputHistoryTests.swift
// （合算を記録のたびに少しずつ直す部分は Windows版では毎回作り直すので、その照合のテストは移していない。
// 変換記録の取り込みは未移植）。
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include <filesystem>
#include <string>
#include <vector>

#include "iroha/input_history.h"

namespace {

std::vector<std::u32string> HistoryUnits(const std::u32string& reading, const std::u32string& text) {
    std::vector<std::u32string> units;
    for (const auto& key : iroha::InputHistoryUnits::Units(reading, text)) {
        units.push_back(key.reading + U"→" + key.text);
    }
    return units;
}

void TestHistoryUnits() {
    using Strings = std::vector<std::u32string>;
    EXPECT_TRUE(HistoryUnits(U"もでるを", U"モデルを") == (Strings{U"もでるを→モデルを", U"もでる→モデル"}));
    EXPECT_TRUE(HistoryUnits(U"かのうなのかな？", U"可能なのかな？") ==
                (Strings{U"かのうなのかな→可能なのかな", U"かのう→可能"}));
    EXPECT_TRUE(HistoryUnits(U"きょうはいいてんきですね", U"今日はいい天気ですね") ==
                (Strings{U"きょうはいい→今日はいい", U"きょう→今日", U"てんきですね→天気ですね", U"てんき→天気"}));
    EXPECT_TRUE(HistoryUnits(U"ちょっと", U"ちょっと") == (Strings{U"ちょっと→ちょっと"}));
    // 1文字の読み・英字の読み・英数の表記は残さない
    EXPECT_TRUE(HistoryUnits(U"の", U"の").empty());
    EXPECT_TRUE(HistoryUnits(U"macos", U"macOS").empty());
    EXPECT_TRUE(HistoryUnits(U"もでる", U"moderu").empty());
}

std::filesystem::path HistoryDirectory() {
    return std::filesystem::temp_directory_path() / ("iroha-input-history-" + iroha::GenerateUuid());
}

std::vector<std::u32string> HistoryTexts(const std::vector<iroha::InputHistoryEntry>& entries) {
    std::vector<std::u32string> texts;
    for (const auto& entry : entries) texts.push_back(entry.text);
    return texts;
}

void TestHistoryCandidates() {
    const auto directory = HistoryDirectory();
    {
        iroha::InputHistoryStore store(directory, "a", 0);
        const int64_t t0 = 1000;
        store.Record(U"もでる", U"モデル", t0);
        store.Record(U"もでる", U"モデル", t0);
        store.Record(U"もでむ", U"モデム", t0 + 10);
        store.Record(U"もでらと", U"モデラート", t0 + 20);
        store.Record(U"もでるが", U"モデルが", t0 + 5);
        // モデルは「モデルが」の語の部分も数えるので 3 回。あとは同じ回数なので最近使った順
        EXPECT_TRUE(HistoryTexts(store.Candidates(U"もで")) ==
                    (std::vector<std::u32string>{U"モデル", U"モデラート", U"モデム"}));
        // 打った読みより長い語だけ
        EXPECT_TRUE(store.Candidates(U"もでらと").empty());
        EXPECT_TRUE(store.Candidates(U"もでx").empty());
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void TestHistoryMergesHostsAndPersists() {
    const auto directory = HistoryDirectory();
    {
        iroha::InputHistoryStore a(directory, "a", 0);
        a.Record(U"もでる", U"モデル", 1000);
        iroha::InputHistoryStore b(directory, "b", 0);
        b.Record(U"もでる", U"モデル", 1000);
        EXPECT_TRUE(b.Candidates(U"もで").size() == 1 && b.Candidates(U"もで")[0].count == 2);
        a.Reload();
        EXPECT_TRUE(a.Candidates(U"もで")[0].count == 2);
        // 自分のファイルには自分の分だけ。時刻は秒で読み書きできる
        iroha::InputHistoryStore reopened(directory, "a", 0);
        EXPECT_TRUE(reopened.Count() == 1 && reopened.Candidates(U"もで")[0].count == 2);
        EXPECT_TRUE(reopened.Candidates(U"もで")[0].lastUsed == 1000);
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void TestHistoryRemoveAndReset() {
    const auto directory = HistoryDirectory();
    {
        iroha::InputHistoryStore a(directory, "a", 0);
        a.Record(U"もでる", U"モデル", 1000);
        iroha::InputHistoryStore b(directory, "b", 0);
        b.Record(U"もでる", U"モデル", 1000);
        b.Remove({{U"もでる", U"モデル"}}, 1001);
        EXPECT_TRUE(b.Candidates(U"もで").empty());
        a.Reload();
        EXPECT_TRUE(a.Candidates(U"もで").empty());
        // 消したあとにまた確定すれば戻る
        a.Record(U"もでる", U"モデル", 1002);
        EXPECT_TRUE(HistoryTexts(a.Candidates(U"もで")) == (std::vector<std::u32string>{U"モデル"}));
        // すべて消すと、それより前に使った分はどの機械のものも数えない
        b.Reset(1003);
        EXPECT_TRUE(b.Count() == 0);
        a.Reload();
        EXPECT_TRUE(a.Count() == 0);
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// 遅らせた書き込みは、捨てるときに済ませる
void TestHistoryDelayedSave() {
    const auto directory = HistoryDirectory();
    {
        iroha::InputHistoryStore store(directory, "a", 60000);
        store.Record(U"もでる", U"モデル", 1000);
    }
    {
        iroha::InputHistoryStore reopened(directory, "a", 0);
        EXPECT_TRUE(reopened.Count() == 1);
    }
    EXPECT_EQ(iroha::Utf8ToUtf32(iroha::InputHistoryStore::FormatTime(0)), U"1970-01-01T00:00:00Z");
    EXPECT_TRUE(iroha::InputHistoryStore::ParseTime("2026-11-13T10:00:00Z") ==
                std::optional<int64_t>(1794564000));
    EXPECT_EQ(iroha::Utf8ToUtf32(iroha::InputHistoryStore::FormatTime(1794564000)), U"2026-11-13T10:00:00Z");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void RunHistoryTests() {
    TestHistoryUnits();
    TestHistoryCandidates();
    TestHistoryMergesHostsAndPersists();
    TestHistoryRemoveAndReset();
    TestHistoryDelayedSave();
}

} // namespace
