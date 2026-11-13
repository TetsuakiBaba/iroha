// 共有の部品（linux/shared）の単体テスト: 起動の記録・描けない絵文字の判定・ショートカット
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "debug_overlay_window.h"
#include "emoji_renderability.h"
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

} // namespace

int main() {
    TestLaunchLog();
    TestEmojiRenderability();
    TestKeyShortcut();
    TestDebugOverlayPlacement();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
