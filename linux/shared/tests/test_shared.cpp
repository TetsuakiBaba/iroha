// 共有の部品（linux/shared）の単体テスト: 起動の記録・描けない絵文字の判定・ショートカット
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "debug_overlay_window.h"
#include "emoji_renderability.h"
#include "input_menu.h"
#include "iroha/config.h"
#include "key_shortcut.h"
#include "launch_log.h"

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

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// 前回の終了が記録されていなければ、次の起動で unclean-exit を書く。終了を記録すれば書かない
void TestLaunchLog() {
    const auto directory = std::filesystem::temp_directory_path() / ("iroha-launch-test-" + std::to_string(getpid()));
    std::filesystem::remove_all(directory);
    const auto crashes = directory / "crash";
    std::filesystem::create_directories(crashes);
    iroha::LaunchLog log(directory, "host", "ibus-engine-iroha");
    auto lines = log.RecordLaunch("1.0", crashes);
    EXPECT_TRUE(lines.size() == 1 && lines[0].rfind("launch ", 0) == 0);
    log.RecordExit("SIGTERM");
    lines = log.RecordLaunch("1.0", crashes);
    EXPECT_TRUE(lines.size() == 1);
    // 終了を記録しないまま、別の（もう動いていない）プロセスとして起動したことにする
    std::string state = ReadAll(log.StatePath());
    const std::string pid = "\"pid\":" + std::to_string(getpid());
    state.replace(state.find(pid), pid.size(), "\"pid\":999999999");
    // 前回のレポートの確認を 1 分前にする（判定は秒の単位なので、同じ秒に作ったレポートは新しいと見なさない）
    const size_t checked = state.find("\"reportsCheckedAt\":") + 19;
    const size_t checkedEnd = state.find_first_of(",}", checked);
    state.replace(checked, checkedEnd - checked, std::to_string(std::stoll(state.substr(checked, checkedEnd - checked)) - 60));
    std::ofstream(log.StatePath(), std::ios::binary | std::ios::trunc) << state;
    // 新しいクラッシュレポート（名前に ibus-engine-iroha を含む .crash）
    std::ofstream(crashes / "_usr_local_lib_iroha_ibus-engine-iroha.1000.crash") << "x";
    std::ofstream(crashes / "_usr_bin_other.1000.crash") << "x";
    lines = log.RecordLaunch("1.0", crashes);
    EXPECT_TRUE(lines.size() == 3 && lines[0].rfind("unclean-exit", 0) == 0 && lines[1].rfind("crash-report", 0) == 0);
    const std::string text = ReadAll(log.LogPath());
    EXPECT_TRUE(text.find("exit reason=SIGTERM") != std::string::npos);
    std::filesystem::remove_all(directory);
}

// 絵文字のフォントがあるときだけ: 1 つの絵になるもの・ならないもの
void TestEmojiRenderability() {
    auto checker = iroha::EmojiRenderability::Create();
    if (!checker) {
        std::printf("skip: 絵文字のフォントが無い\n");
        return;
    }
    EXPECT_TRUE(checker->IsRenderable(U"😀"));
    EXPECT_TRUE(checker->IsRenderable(U"✅️"));         // ✅️（異体字セレクタは字形に数えない）
    EXPECT_TRUE(checker->IsRenderable(U"🐈‍⬛"));            // 黒猫（ZWJ でつないだ組）
    EXPECT_TRUE(checker->IsRenderable(U"\U0001F1EF\U0001F1F5")); // 日本の国旗
    EXPECT_TRUE(!checker->IsRenderable(U"A"));                    // 絵文字のフォントに無い
    EXPECT_TRUE(!checker->IsRenderable(U"😀😀"));                 // 2 つの絵になる
}

void TestKeyShortcut() {
    const auto shortcut = iroha::KeyShortcut::Parse("Ctrl+Alt+A");
    EXPECT_TRUE(shortcut && shortcut->Keysym() == 'a');
    EXPECT_TRUE(shortcut->Matches('a', (1u << 2) | (1u << 3)));
    EXPECT_TRUE(!shortcut->Matches('a', 1u << 2));
    const auto enter = iroha::KeyShortcut::Parse("Alt+Enter");
    EXPECT_TRUE(enter && enter->Keysym() == 0xff0d && enter->Matches(0xff8d, 1u << 3));
    const auto f2 = iroha::KeyShortcut::Parse("Shift+F2");
    EXPECT_TRUE(f2 && f2->Keysym() == 0xffbf && f2->Matches(0xffbf, 1u << 0));
    EXPECT_TRUE(!iroha::KeyShortcut::Parse("A"));  // 修飾キーの無いものは受けない
}

// デバッグ表示の置き場所: カーソルの右下。下に収まらない・候補ウィンドウが出ているときは行の上。画面の右端で止める
void TestDebugOverlayPlacement() {
    const iroha::DebugOverlayWindow::Rect caret{100, 200, 2, 20};
    auto frame = iroha::PlaceDebugOverlay(caret, 300, 40, 1920, 1080, false);
    EXPECT_TRUE(frame.x == 104 && frame.y == 222);
    frame = iroha::PlaceDebugOverlay(caret, 300, 40, 1920, 1080, true);
    EXPECT_TRUE(frame.x == 104 && frame.y == 158);
    frame = iroha::PlaceDebugOverlay({1800, 1050, 2, 20}, 300, 40, 1920, 1080, false);
    EXPECT_TRUE(frame.x == 1620 && frame.y == 1008);
    frame = iroha::PlaceDebugOverlay({10, 10, 2, 20}, 300, 40, 1920, 1080, true);  // 上に収まらなければ下
    EXPECT_TRUE(frame.y == 32);
}

// 入力メニュー: macOS版と同じ並び。切り替えのキーは今と反対のモードの項目に付く。モデルが無いときだけ状況の行
void TestInputMenu() {
    using iroha::menu::Command;
    using Type = iroha::menu::Item::Type;
    iroha::menu::State state;
    state.inputModeShortcut = "Ctrl+Space";
    state.privateModeShortcut = "Ctrl+Alt+P";
    state.version = "0.18.0";
    auto items = iroha::menu::Build(state);
    std::string order;
    for (const auto& item : items) order += (item.type == Type::Separator ? std::string("-") : item.key) + " ";
    EXPECT_TRUE(order ==
                "InputMode InputMode.Direct - Setup UserDictionary RewriteRules - PrivateMode LiveConversion Predictive "
                "Punctuation - ModelFolder - Version ");
    EXPECT_TRUE(items[0].label == "かな" && items[0].checked && items[0].type == Type::Radio);
    EXPECT_TRUE(items[1].label == "英数（Ctrl+Space）" && !items[1].checked);
    EXPECT_TRUE(items[7].label == "プライベートモード（Ctrl+Alt+P）" && !items[7].checked);
    EXPECT_TRUE(items[8].checked && !items[9].checked);  // ライブ変換は既定 ON、予測変換は OFF
    EXPECT_TRUE(items[10].label == "句読点スタイル: 、。（Ctrl+.）");
    EXPECT_TRUE(items.back().label == "iroha 0.18.0" && items.back().type == Type::Status);
    // キーの名前は重ならない（fcitx5 のアクションの名前に使う）
    for (size_t i = 0; i < items.size(); ++i) {
        for (size_t j = i + 1; j < items.size(); ++j) EXPECT_TRUE(items[i].key != items[j].key);
    }
    state.directMode = true;
    state.inputModeShortcut.clear();
    state.privateMode = true;
    state.punctuation = "，．";
    state.modelMissing = true;
    items = iroha::menu::Build(state);
    EXPECT_TRUE(items[0].label == "かな" && !items[0].checked && items[1].label == "英数" && items[1].checked);
    EXPECT_TRUE(items[7].checked);
    EXPECT_TRUE(items[10].label == "句読点スタイル: ，．（Ctrl+.）");
    EXPECT_TRUE(items[items.size() - 2].key == "ModelStatus" && items[items.size() - 2].type == Type::Status);
    EXPECT_TRUE(iroha::menu::CommandFor("InputMode") == Command::KanaMode);
    EXPECT_TRUE(iroha::menu::CommandFor("InputMode.Direct") == Command::DirectMode);
    EXPECT_TRUE(iroha::menu::CommandFor("Punctuation") == Command::Punctuation);
    EXPECT_TRUE(iroha::menu::CommandFor("Version") == Command::None);
    EXPECT_TRUE(iroha::menu::CommandFor("Separator1") == Command::None);
    // かな・英数を切り替えるキーの設定: 既定は Ctrl+Space、空（割り当てなし）も保存して読み戻せる
    const auto path = std::filesystem::temp_directory_path() / ("iroha-config-test-" + std::to_string(getpid()) + ".json");
    iroha::Config config;
    EXPECT_TRUE(config.inputModeHotkey == "Ctrl+Space");
    config.inputModeHotkey = "";
    EXPECT_TRUE(iroha::SaveConfig(path, config));
    EXPECT_TRUE(iroha::LoadConfig(path).inputModeHotkey.empty());
    config.inputModeHotkey = "Ctrl+Shift+J";
    EXPECT_TRUE(iroha::SaveConfig(path, config));
    EXPECT_TRUE(iroha::LoadConfig(path).inputModeHotkey == "Ctrl+Shift+J");
    std::filesystem::remove(path);
}

} // namespace

int main() {
    TestLaunchLog();
    TestEmojiRenderability();
    TestKeyShortcut();
    TestDebugOverlayPlacement();
    TestInputMenu();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
