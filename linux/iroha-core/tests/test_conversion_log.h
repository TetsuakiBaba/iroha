#pragma once
// 変換記録（学習用データのログ）のテスト。
// 移植元: macos/Sources/IrohaCore/ConversionLog.swift と macos/Sources/iroha/ConversionLogSettings.swift
// （形式は macOS版の JSONEncoder の出力と同じにする）。
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "iroha/conversion_log.h"
#include "iroha/input_history.h"

namespace {

iroha::ConversionLogEntry SampleConversion() {
    iroha::ConversionLogEntry entry = iroha::ConversionLogEntry::Make(
        iroha::ConversionLogEntry::Mode::Segments, U"駅で", iroha::ConversionLogEntry::ContextSource::Document,
        U"きしゃ", std::u32string(U"記者"), U"汽車",
        std::vector<iroha::ConversionLogEntry::Segment>{{U"きしゃ", U"汽車"}});
    entry.timestamp = *iroha::InputHistoryStore::ParseTime("2026-11-13T01:02:03Z");
    entry.model = "iroha-kkc-v1-Q5_K_M";
    return entry;
}

// 1行の JSON はキーの名前順で、無い項目は書かない（macOS版と同じ）
void TestConversionLogEncode() {
    const iroha::ConversionLogEntry entry = SampleConversion();
    EXPECT_TRUE(entry.edited == std::optional<bool>(true));
    const std::string line = entry.Encode();
    EXPECT_EQ(iroha::Utf8ToUtf32(line),
              U"{\"committed\":\"汽車\",\"context\":\"駅で\",\"contextSource\":\"document\",\"edited\":true,"
              U"\"format\":1,\"mode\":\"segments\",\"model\":\"iroha-kkc-v1-Q5_K_M\",\"proposed\":\"記者\","
              U"\"reading\":\"きしゃ\",\"segments\":[{\"reading\":\"きしゃ\",\"result\":\"汽車\"}],"
              U"\"timestamp\":\"2026-11-13T01:02:03Z\"}");
    const auto decoded = iroha::ConversionLogEntry::Decode(line);
    EXPECT_TRUE(decoded && *decoded == entry);

    // 提示が不明な記録（proposed・edited が無い）と、壊れた行
    const auto unknown = iroha::ConversionLogEntry::Decode(
        "{\"committed\":\"今日\",\"context\":\"\",\"contextSource\":\"none\",\"format\":1,\"mode\":\"live\","
        "\"model\":\"zenz\",\"reading\":\"きょう\",\"timestamp\":\"2026-09-01T00:00:00Z\"}");
    EXPECT_TRUE(unknown && !unknown->proposed && !unknown->edited && !unknown->segments);
    EXPECT_TRUE(unknown && unknown->contextSource == iroha::ConversionLogEntry::ContextSource::None);
    EXPECT_TRUE(!iroha::ConversionLogEntry::Decode("{\"committed\":"));
}

// 左文脈のない確定・読みをそのまま確定した1文字は残さない。「直した確定だけ」ではそのまま確定したものを残さない
void TestConversionLogShouldRecord() {
    using Entry = iroha::ConversionLogEntry;
    const auto make = [](std::u32string context, std::u32string reading, std::optional<std::u32string> proposed,
                         std::u32string committed) {
        return Entry::Make(Entry::Mode::Live, std::move(context), Entry::ContextSource::Document, std::move(reading),
                           std::move(proposed), std::move(committed));
    };
    const auto all = iroha::ConversionLogScope::All;
    const auto corrections = iroha::ConversionLogScope::Corrections;
    EXPECT_TRUE(iroha::ShouldRecordConversion(make(U"駅で", U"きしゃ", U"記者", U"記者"), all));
    EXPECT_TRUE(!iroha::ShouldRecordConversion(make(U"", U"きしゃ", U"記者", U"記者"), all));
    EXPECT_TRUE(!iroha::ShouldRecordConversion(make(U"駅で", U"の", U"の", U"の"), all));
    // 2 文字のかなの確定は「あえて漢字にしない」例なので残す
    EXPECT_TRUE(iroha::ShouldRecordConversion(make(U"駅で", U"こと", U"こと", U"こと"), all));
    EXPECT_TRUE(!iroha::ShouldRecordConversion(make(U"駅で", U"きしゃ", U"記者", U"記者"), corrections));
    EXPECT_TRUE(iroha::ShouldRecordConversion(make(U"駅で", U"きしゃ", U"記者", U"汽車"), corrections));
    // 提示が不明なら直したかどうか分からないので残す
    EXPECT_TRUE(iroha::ShouldRecordConversion(make(U"駅で", U"きしゃ", std::nullopt, U"汽車"), corrections));
}

// 月ごとのファイルに追記し、一覧（時系列）と削除ができる
void TestConversionLogFiles() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("iroha-conversion-log-" + iroha::GenerateUuid());
    iroha::ConversionLog log(directory, "host");
    iroha::ConversionLogEntry later = SampleConversion();
    iroha::ConversionLogEntry earlier = SampleConversion();
    earlier.timestamp -= 60;
    earlier.committed = U"記者";
    earlier.edited = false;
    log.Record(later);
    log.Record(earlier);
    const auto files = log.Files();
    EXPECT_TRUE(files.size() == 1);
    EXPECT_TRUE(!files.empty() && files[0].filename().string().rfind("conversions-host-2026-11", 0) == 0);
    auto items = log.Items();
    EXPECT_TRUE(items.size() == 2 && items[0].entry == earlier && items[1].entry == later);
    EXPECT_TRUE(items.size() == 2 && items[0].line == 1 && items[1].line == 0);
    log.Delete({items[0]});
    items = log.Items();
    EXPECT_TRUE(items.size() == 1 && items[0].entry == later && items[0].line == 0);
    log.Delete(items);
    EXPECT_TRUE(log.Files().empty()); // 空になったファイルは消す
    log.Record(later);
    log.RemoveAll();
    EXPECT_TRUE(log.Files().empty() && log.TotalSize() == 0);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

void RunConversionLogTests() {
    TestConversionLogEncode();
    TestConversionLogShouldRecord();
    TestConversionLogFiles();
}

} // namespace
