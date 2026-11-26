#pragma once
// 文字の種類を変える Control のキーの系列のテスト（macOS版 KanaTypeKeysTests.swift と同じ組み合わせ）。
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include <set>

#include "iroha/config.h"
#include "iroha/input_session.h"
#include "iroha/kana_type_keys.h"
#include "ipc_protocol.h"

namespace {

bool ConversionIs(iroha::KanaTypeKeyStyle style, char32_t character, iroha::KanaTypeConversion expected) {
    const auto conversion = iroha::KanaTypeConversionForControlKey(style, character);
    return conversion && *conversion == expected;
}

bool NoConversion(iroha::KanaTypeKeyStyle style, char32_t character) {
    return !iroha::KanaTypeConversionForControlKey(style, character).has_value();
}

void TestKanaTypeKeysWindowsStyle() {
    using iroha::KanaTypeConversion;
    const auto style = iroha::KanaTypeKeyStyle::Windows;
    EXPECT_TRUE(ConversionIs(style, U'u', KanaTypeConversion::Hiragana));
    EXPECT_TRUE(ConversionIs(style, U'i', KanaTypeConversion::Katakana));
    EXPECT_TRUE(ConversionIs(style, U'o', KanaTypeConversion::HalfWidthKatakana));
    EXPECT_TRUE(ConversionIs(style, U'p', KanaTypeConversion::FullWidthAlphanumeric));
    EXPECT_TRUE(ConversionIs(style, U't', KanaTypeConversion::HalfWidthAlphanumeric));
    EXPECT_TRUE(NoConversion(style, U'k'));
}

void TestKanaTypeKeysKotoeriStyle() {
    using iroha::KanaTypeConversion;
    const auto style = iroha::KanaTypeKeyStyle::Kotoeri;
    EXPECT_TRUE(ConversionIs(style, U'j', KanaTypeConversion::Hiragana));
    EXPECT_TRUE(ConversionIs(style, U'k', KanaTypeConversion::Katakana));
    EXPECT_TRUE(ConversionIs(style, U';', KanaTypeConversion::HalfWidthKatakana));
    EXPECT_TRUE(ConversionIs(style, U'l', KanaTypeConversion::FullWidthAlphanumeric));
    EXPECT_TRUE(ConversionIs(style, U':', KanaTypeConversion::HalfWidthAlphanumeric));
    EXPECT_TRUE(ConversionIs(style, U'\'', KanaTypeConversion::HalfWidthAlphanumeric));
    EXPECT_TRUE(NoConversion(style, U'u'));
    EXPECT_TRUE(NoConversion(style, U'i'));
}

void TestKanaTypeKeysAtokStyle() {
    using iroha::KanaTypeConversion;
    const auto style = iroha::KanaTypeKeyStyle::Atok;
    EXPECT_TRUE(ConversionIs(style, U'u', KanaTypeConversion::Hiragana));
    EXPECT_TRUE(ConversionIs(style, U'@', KanaTypeConversion::HalfWidthAlphanumeric));
    EXPECT_TRUE(NoConversion(style, U't'));
}

// どの系列も 5 種類の変換をすべて持つ
void TestKanaTypeKeysEveryStyleCoversAllConversions() {
    const std::u32string keys = U"abcdefghijklmnopqrstuvwxyz;:'@";
    for (const auto style : {iroha::KanaTypeKeyStyle::Windows, iroha::KanaTypeKeyStyle::Kotoeri,
                             iroha::KanaTypeKeyStyle::Atok}) {
        std::set<int> covered;
        for (const char32_t key : keys) {
            if (const auto conversion = iroha::KanaTypeConversionForControlKey(style, key)) {
                covered.insert(static_cast<int>(*conversion));
            }
        }
        EXPECT_TRUE(covered.size() == 5);
    }
}

// Shift つきは扱わない（Ctrl+Shift+J など）。@ だけは英語配列で Shift+2 なので受ける
void TestKanaTypeKeysShift() {
    using iroha::KanaTypeConversion;
    using iroha::KanaTypeKeyStyle;
    EXPECT_TRUE(!iroha::KanaTypeConversionForControlKey(KanaTypeKeyStyle::Kotoeri, U'j', true));
    EXPECT_TRUE(!iroha::KanaTypeConversionForControlKey(KanaTypeKeyStyle::Windows, U'u', true));
    EXPECT_TRUE(!iroha::KanaTypeConversionForControlKey(KanaTypeKeyStyle::Kotoeri, U':', true));
    const auto atShift = iroha::KanaTypeConversionForControlKey(KanaTypeKeyStyle::Atok, U'@', true);
    EXPECT_TRUE(atShift && *atShift == KanaTypeConversion::HalfWidthAlphanumeric);
    const auto plain = iroha::KanaTypeConversionForControlKey(KanaTypeKeyStyle::Windows, U'u', false);
    EXPECT_TRUE(plain && *plain == KanaTypeConversion::Hiragana);
}

// 設定の値: 知らない値・空なら windows。保存して読み直すと残る
void TestKanaTypeKeyStyleSetting() {
    using iroha::KanaTypeKeyStyle;
    EXPECT_TRUE(iroha::ParseKanaTypeKeyStyle("windows") == KanaTypeKeyStyle::Windows);
    EXPECT_TRUE(iroha::ParseKanaTypeKeyStyle("kotoeri") == KanaTypeKeyStyle::Kotoeri);
    EXPECT_TRUE(iroha::ParseKanaTypeKeyStyle("atok") == KanaTypeKeyStyle::Atok);
    EXPECT_TRUE(iroha::ParseKanaTypeKeyStyle("") == KanaTypeKeyStyle::Windows);
    EXPECT_TRUE(iroha::ParseKanaTypeKeyStyle("emacs") == KanaTypeKeyStyle::Windows);
    for (const auto style : {KanaTypeKeyStyle::Windows, KanaTypeKeyStyle::Kotoeri, KanaTypeKeyStyle::Atok}) {
        EXPECT_TRUE(iroha::ParseKanaTypeKeyStyle(iroha::KanaTypeKeyStyleName(style)) == style);
    }

    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("iroha-kana-type-" + iroha::GenerateUuid());
    const std::filesystem::path path = directory / "config.json";
    iroha::Config config = iroha::LoadConfig(path);
    EXPECT_TRUE(config.kanaTypeKeyStyle == KanaTypeKeyStyle::Windows);
    config.kanaTypeKeyStyle = KanaTypeKeyStyle::Kotoeri;
    EXPECT_TRUE(iroha::SaveConfig(path, config));
    EXPECT_TRUE(iroha::LoadConfig(path).kanaTypeKeyStyle == KanaTypeKeyStyle::Kotoeri);
    // 知らない値は windows
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "{\"kanaTypeKeyStyle\":\"emacs\"}";
    }
    EXPECT_TRUE(iroha::LoadConfig(path).kanaTypeKeyStyle == KanaTypeKeyStyle::Windows);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// 変換サーバの返事に載せた系列が TIP に届く（返事を作って読み直す）
void TestKanaTypeKeyStyleInSessionResponse() {
    iroha::SessionOutput output;
    output.kanaTypeKeyStyle = iroha::KanaTypeKeyStyle::Atok;
    output.inputModeShortcut = 0x10020;
    const iroha::ipc::SessionResponse response =
        iroha::ipc::ParseSessionResponse(iroha::ipc::BuildSessionResponse(&output));
    EXPECT_TRUE(response.ok && response.changed);
    EXPECT_TRUE(response.output.kanaTypeKeyStyle == iroha::KanaTypeKeyStyle::Atok);
    EXPECT_TRUE(response.output.inputModeShortcut == 0x10020);
}

} // namespace

void RunKanaTypeKeyTests() {
    TestKanaTypeKeysWindowsStyle();
    TestKanaTypeKeysKotoeriStyle();
    TestKanaTypeKeysAtokStyle();
    TestKanaTypeKeysEveryStyleCoversAllConversions();
    TestKanaTypeKeysShift();
    TestKanaTypeKeyStyleSetting();
    TestKanaTypeKeyStyleInSessionResponse();
}
