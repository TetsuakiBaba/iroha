// 選択テキストのAI編集の規則（selection_rules）のテスト。画面と X を使わない
#include <cstdio>

#include "iroha/unicode.h"
#include "selection_rules.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void Expect(bool condition, const char* expression, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}
#define EXPECT_TRUE(cond) Expect((cond), #cond, __LINE__)

namespace s = iroha::selection;
using iroha::Config;

void TestSettingsFromConfig() {
    // 書いていない項目は既定（プリセット 1〜4 は Ctrl+Alt+1〜4、5 つ目は空で無効。その場で指示するキーは Ctrl+Alt+0）
    Config config;
    s::Settings settings = s::SettingsFromConfig(config);
    EXPECT_TRUE(!settings.enabled);
    EXPECT_TRUE(settings.presets.size() == 5);
    EXPECT_TRUE(settings.presets[0].name == U"日英翻訳" && settings.presets[0].hotkey == "Ctrl+Alt+1");
    EXPECT_TRUE(settings.presets[3].hotkey == "Ctrl+Alt+4" && settings.presets[3].enabled);
    EXPECT_TRUE(!settings.presets[4].enabled && settings.presets[4].hotkey.empty());
    EXPECT_TRUE(settings.onDemandHotkey == "Ctrl+Alt+0");

    config.selectionEnabled = true;
    config.selectionPresets[1].name = U"直す";
    config.selectionPresets[1].hotkey = "Shift+Alt+F2";
    config.selectionPresets[2].enabled = false;
    config.selectionOnDemandHotkey = "";
    config.selectionExcludedApps = " Firefox, gnome-terminal-\n\nKeePassXC ";
    settings = s::SettingsFromConfig(config);
    EXPECT_TRUE(settings.enabled);
    EXPECT_TRUE(settings.presets[1].name == U"直す" && settings.presets[1].hotkey == "Shift+Alt+F2");
    // 名前だけ変えたプリセットのプロンプトは既定のまま
    EXPECT_TRUE(settings.presets[1].prompt == iroha::SelectionSettingsDefaults::Presets()[1].prompt);
    EXPECT_TRUE(!settings.presets[2].enabled);
    EXPECT_TRUE(settings.onDemandHotkey.empty());
    EXPECT_TRUE(settings.excludedApps.size() == 3 && settings.excludedApps[0] == "firefox" &&
                settings.excludedApps[1] == "gnome-terminal-" && settings.excludedApps[2] == "keepassxc");
}

void TestExcludedApps() {
    EXPECT_TRUE(s::ParseExcludedApps("").empty());
    EXPECT_TRUE(s::ParseExcludedApps(" , \n").empty());
    // 重複は 1 つにする・改行（CRLF も）とカンマで区切る
    const auto names = s::ParseExcludedApps("Code\r\ncode,Slack");
    EXPECT_TRUE(names.size() == 2 && names[0] == "code" && names[1] == "slack");

    const std::vector<std::string> excluded = {"firefox", "keepassxc"};
    // /proc/<pid>/comm で当たる
    EXPECT_TRUE(s::IsExcluded(excluded, {"firefox", "", ""}));
    // WM_CLASS で当たる（大文字小文字は見ない）
    EXPECT_TRUE(s::IsExcluded(excluded, {"", "keepassxc", "KeePassXC"}));
    EXPECT_TRUE(s::IsExcluded(excluded, {"", "", "KeePassXC"}));
    EXPECT_TRUE(!s::IsExcluded(excluded, {"gedit", "gedit", "Gedit"}));
    EXPECT_TRUE(!s::IsExcluded({}, {"gedit", "gedit", "Gedit"}));
    // iroha 自身（設定画面・この常駐プログラム）では使わない
    EXPECT_TRUE(s::IsExcluded({}, {"iroha-selection", "", ""}));
    EXPECT_TRUE(s::IsExcluded({}, {"", "iroha-settings", "Iroha-settings"}));
    // 名前が取れないアプリは使う
    EXPECT_TRUE(!s::IsExcluded(excluded, {"", "", ""}));
}

void TestHotkeys() {
    // ショートカットの文字列 → X の keysym と修飾キー
    const auto ctrlAlt1 = iroha::KeyShortcut::Parse("Ctrl+Alt+1");
    EXPECT_TRUE(ctrlAlt1 && ctrlAlt1->Keysym() == '1' && s::ModifierMask(*ctrlAlt1) == (s::kControlMask | s::kAltMask));
    const auto shiftF5 = iroha::KeyShortcut::Parse("Shift+F5");
    EXPECT_TRUE(shiftF5 && shiftF5->Keysym() == 0xffc2 && s::ModifierMask(*shiftF5) == s::kShiftMask);
    const auto optionA = iroha::KeyShortcut::Parse("Option+A");
    EXPECT_TRUE(optionA && optionA->Keysym() == 'a' && s::ModifierMask(*optionA) == s::kAltMask);
    const auto ctrlSpace = iroha::KeyShortcut::Parse("Control+Space");
    EXPECT_TRUE(ctrlSpace && ctrlSpace->Keysym() == 0x20 && s::ModifierMask(*ctrlSpace) == s::kControlMask);

    Config config;
    config.selectionEnabled = true;
    config.selectionPresets[1].hotkey = "Ctrl+Alt+1"; // 1 つ目と同じキー（先勝ち）
    config.selectionPresets[2].hotkey = "Cmd+3";      // 読めない
    config.selectionPresets[3].enabled = false;       // 無効なプリセットのキーはつかまない
    config.selectionPresets[4].hotkey = "Ctrl+Shift+F12";
    config.selectionPresets[4].enabled = true;
    s::Settings settings = s::SettingsFromConfig(config);
    std::vector<s::HotkeyBinding> bindings = s::PlanHotkeys(settings);
    EXPECT_TRUE(bindings.size() == 3);
    EXPECT_TRUE(bindings[0].id == 0 && bindings[0].keysym == '1' && bindings[0].modifiers == (s::kControlMask | s::kAltMask));
    EXPECT_TRUE(bindings[1].id == 4 && bindings[1].keysym == 0xffc9 &&
                bindings[1].modifiers == (s::kControlMask | s::kShiftMask));
    EXPECT_TRUE(bindings[2].id == s::kOnDemandId && bindings[2].keysym == '0' && bindings[2].text == "Ctrl+Alt+0");

    // その場で指示するキーがプリセットと同じなら、プリセットが先
    config.selectionOnDemandHotkey = "ctrl+alt+1";
    bindings = s::PlanHotkeys(s::SettingsFromConfig(config));
    EXPECT_TRUE(bindings.size() == 2 && bindings[0].id == 0 && bindings[1].id == 4);

    // OFF なら何もつかまない
    config.selectionEnabled = false;
    EXPECT_TRUE(s::PlanHotkeys(s::SettingsFromConfig(config)).empty());

    // NumLock（ふつう Mod2 = 1<<4）・CapsLock の組み合わせ
    const unsigned mod2 = 1u << 4;
    const auto variants = s::LockVariants(s::kControlMask, mod2);
    EXPECT_TRUE(variants.size() == 4);
    EXPECT_TRUE(variants[0] == s::kControlMask && variants[1] == (s::kControlMask | s::kLockMask) &&
                variants[2] == (s::kControlMask | mod2) && variants[3] == (s::kControlMask | mod2 | s::kLockMask));
    EXPECT_TRUE(s::LockVariants(s::kAltMask, 0).size() == 2);

    // 届いたキーの照合（NumLock・CapsLock は見ない。修飾キーが多くても少なくても当たらない）
    const std::vector<s::GrabbedKey> keys = {{0, 10, s::kControlMask | s::kAltMask}, {s::kOnDemandId, 19, s::kControlMask | s::kAltMask}};
    EXPECT_TRUE(s::FindHotkey(keys, 10, s::kControlMask | s::kAltMask, mod2) == 0);
    EXPECT_TRUE(s::FindHotkey(keys, 10, s::kControlMask | s::kAltMask | mod2 | s::kLockMask, mod2) == 0);
    EXPECT_TRUE(s::FindHotkey(keys, 19, s::kControlMask | s::kAltMask, mod2) == s::kOnDemandId);
    EXPECT_TRUE(s::FindHotkey(keys, 10, s::kControlMask, mod2) == -1);
    EXPECT_TRUE(s::FindHotkey(keys, 10, s::kControlMask | s::kAltMask | s::kShiftMask, mod2) == -1);
    EXPECT_TRUE(s::FindHotkey(keys, 11, s::kControlMask | s::kAltMask, mod2) == -1);
}

void TestRequests() {
    const auto& defaults = iroha::SelectionSettingsDefaults::Presets();
    s::Settings::Preset preset{U"", U"", "Ctrl+Alt+2", true};
    // 名前が空なら「プリセットN」、プロンプトが空なら既定
    EXPECT_TRUE(s::PresetHeading(preset, 1) == U"プリセット2");
    EXPECT_TRUE(s::PresetHeading({U" 校正 ", U"", "", true}, 1) == U"校正");
    iroha::AIRequest request = s::PresetRequest(preset, 1, U"こんにちわ");
    EXPECT_TRUE(request.instructions.find(defaults[1].prompt) == 0);
    EXPECT_TRUE(request.userMessage == U"こんにちわ");
    // 5 つ目（カスタム）は既定のプロンプトも空
    request = s::PresetRequest({U"カスタム", U"", "", true}, 4, U"本文");
    EXPECT_TRUE(request.userMessage == U"本文");
    // {text} があればその位置に差し込む
    request = s::PresetRequest({U"x", U"「{text}」を英訳", "", true}, 0, U"猫");
    EXPECT_TRUE(request.userMessage == U"「猫」を英訳");
    EXPECT_TRUE(request.instructions == iroha::AICommitSettings::kOutputRule);

    // その場の指示: 選択があれば指示 + 選択、無ければ文章を作る
    request = s::PromptRequest(U"箇条書きにして", U"本文", false);
    EXPECT_TRUE(request.instructions.find(U"箇条書きにして") == 0 && request.userMessage == U"本文");
    request = s::PromptRequest(U"お礼のメール", U"", true);
    EXPECT_TRUE(request.instructions.find(iroha::SelectionSettingsDefaults::kGenerationInstructions) == 0);
    EXPECT_TRUE(request.userMessage == U"お礼のメール");

    EXPECT_TRUE(s::SourceLine(U"一行目\n二行目\tです", false) == U"選択: 一行目 二行目 です");
    EXPECT_TRUE(s::SourceLine(U"", true).find(U"選択なし") != std::u32string::npos);

    // PRIMARY 選択: 無い・空白だけなら選択なし
    EXPECT_TRUE(!s::SelectionFromPrimary(std::nullopt));
    EXPECT_TRUE(!s::SelectionFromPrimary(std::string(" \n\t")));
    const auto text = s::SelectionFromPrimary(std::string(" 日本語 "));
    EXPECT_TRUE(text && *text == U" 日本語 ");
}

// マウスで選んだときの出し方と文字数（既定はアイコン・文字数なし）
void TestMouseSettings() {
    Config config;
    s::Settings settings = s::SettingsFromConfig(config);
    EXPECT_TRUE(settings.trigger == s::Settings::Trigger::Bubble && !settings.characterCount);
    // AI編集が OFF なら、文字数を出さない限りマウスを見ない
    EXPECT_TRUE(!settings.WatchesMouse() && !settings.MouseTriggersAI(false));
    config.selectionCharacterCount = true;
    settings = s::SettingsFromConfig(config);
    EXPECT_TRUE(settings.WatchesMouse() && !settings.MouseTriggersAI(false));
    config.selectionEnabled = true;
    config.selectionCharacterCount = false;
    config.selectionTriggerMode = "optionMenu";
    settings = s::SettingsFromConfig(config);
    EXPECT_TRUE(settings.trigger == s::Settings::Trigger::AltMenu && settings.WatchesMouse());
    EXPECT_TRUE(!settings.MouseTriggersAI(false) && settings.MouseTriggersAI(true));
    config.selectionTriggerMode = "off";
    settings = s::SettingsFromConfig(config);
    EXPECT_TRUE(!settings.WatchesMouse() && !settings.MouseTriggersAI(true));
    EXPECT_TRUE(s::ParseTrigger("immediateMenu") == s::Settings::Trigger::ImmediateMenu);
    EXPECT_TRUE(s::ParseTrigger("unknown") == s::Settings::Trigger::Bubble);
}

// ドラッグ（4 ピクセル以上）とダブルクリック（400ms・5 ピクセル以内）。3 回目のクリックは数えない
void TestGestures() {
    s::GestureTracker tracker(4, 400, 5);
    tracker.Press(100, 100);
    EXPECT_TRUE(!tracker.Release(102, 101, 1000));  // ただのクリック
    tracker.Press(102, 101);
    EXPECT_TRUE(tracker.Release(102, 101, 1300));   // ダブルクリック
    tracker.Press(102, 101);
    EXPECT_TRUE(!tracker.Release(102, 101, 1450));  // 3 回目
    tracker.Press(10, 10);
    EXPECT_TRUE(tracker.Release(14, 10, 5000));     // ドラッグ
    tracker.Press(14, 10);
    EXPECT_TRUE(!tracker.Release(14, 10, 5100));    // ドラッグの直後のクリックはダブルクリックにしない
    tracker.Press(14, 10);
    EXPECT_TRUE(!tracker.Release(14, 10, 5600));    // 間隔が長い
    tracker.Press(30, 10);
    EXPECT_TRUE(!tracker.Release(30, 10, 5700));    // 離れた場所
    EXPECT_TRUE(!tracker.Release(60, 60, 5800));    // 押していないのに離した
}

void TestMouseSelection() {
    EXPECT_TRUE(s::ShouldUseMouseSelection(U"文章", true, true, U""));
    EXPECT_TRUE(!s::ShouldUseMouseSelection(U"文章", true, false, U""));   // 持ち主が変わっていない
    EXPECT_TRUE(!s::ShouldUseMouseSelection(U" \n", true, true, U""));    // 空白だけ
    EXPECT_TRUE(!s::ShouldUseMouseSelection(std::nullopt, true, true, U""));
    EXPECT_TRUE(s::ShouldUseMouseSelection(U"新しい", false, false, U"前の"));
    EXPECT_TRUE(!s::ShouldUseMouseSelection(U"前の", false, false, U"前の"));
}

} // namespace

int main() {
    TestSettingsFromConfig();
    TestExcludedApps();
    TestHotkeys();
    TestRequests();
    TestMouseSettings();
    TestGestures();
    TestMouseSelection();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
