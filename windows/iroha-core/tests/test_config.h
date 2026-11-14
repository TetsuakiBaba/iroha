#pragma once
// config.json の読み書きのテスト（既定値と、保存して読み直したときに値が残るか）。
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include <fstream>

#include "iroha/config.h"

namespace {

void TestConfigDefaultsAndRoundTrip() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / ("iroha-config-" + iroha::GenerateUuid());
    const std::filesystem::path path = directory / "config.json";
    // ファイルが無ければ既定値
    iroha::Config config = iroha::LoadConfig(path);
    EXPECT_TRUE(config.inputModeHotkey == "Ctrl+Space");
    EXPECT_TRUE(config.privateModeHotkey == "Ctrl+Alt+P");
    EXPECT_TRUE(config.autoUpdateCheck && config.updateCheckURL.empty());
    EXPECT_TRUE(!config.developerMode && !config.developerOverlay);

    config.inputModeHotkey = "";  // 割り当てなし
    config.autoUpdateCheck = false;
    config.updateCheckURL = "file:///C:/feed.json";
    config.developerMode = true;
    config.developerOverlay = true;
    EXPECT_TRUE(iroha::SaveConfig(path, config));
    const iroha::Config loaded = iroha::LoadConfig(path);
    EXPECT_TRUE(loaded.inputModeHotkey.empty());
    EXPECT_TRUE(!loaded.autoUpdateCheck && loaded.updateCheckURL == "file:///C:/feed.json");
    EXPECT_TRUE(loaded.developerMode && loaded.developerOverlay);

    // 書いていない項目は既定値（以前の版が書いた config.json）
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "{\"liveConversion\":false,\"version\":1}";
    }
    const iroha::Config old = iroha::LoadConfig(path);
    EXPECT_TRUE(!old.liveConversion && old.inputModeHotkey == "Ctrl+Space" && old.autoUpdateCheck);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// 「こえ」モードのキー: 押している間のキーの名前 → 仮想キーコード、押して始め・終えるキーの既定値と保存
void TestConfigVoiceKeys() {
    EXPECT_TRUE(iroha::VoiceHoldKeyCode("rightControl") == 0xA3);
    EXPECT_TRUE(iroha::VoiceHoldKeyCode("rightShift") == 0xA1);
    EXPECT_TRUE(iroha::VoiceHoldKeyCode("kana") == iroha::kVoiceKeyKana);
    EXPECT_TRUE(iroha::VoiceHoldKeyCode("none") == 0);
    EXPECT_TRUE(iroha::VoiceHoldKeyCode("unknown") == 0xA3);

    const std::filesystem::path directory = std::filesystem::temp_directory_path() / ("iroha-config-" + iroha::GenerateUuid());
    const std::filesystem::path path = directory / "config.json";
    iroha::Config config = iroha::LoadConfig(path);
    EXPECT_TRUE(config.voiceInputHoldKey == "rightControl" && config.voiceInputToggleHotkey == "Ctrl+Alt+K");
    config.voiceInputHoldKey = "kana";
    config.voiceInputToggleHotkey = ""; // 割り当てなし
    EXPECT_TRUE(iroha::SaveConfig(path, config));
    const iroha::Config loaded = iroha::LoadConfig(path);
    EXPECT_TRUE(loaded.voiceInputHoldKey == "kana" && loaded.voiceInputToggleHotkey.empty());
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void RunConfigTests() {
    TestConfigDefaultsAndRoundTrip();
    TestConfigVoiceKeys();
}

} // namespace
