// キーの読み替え（key_decision）のテスト。規則は Windows版 TIP の DecideKey と同じ
#include <cstdio>

#include "key_decision.h"

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

using iroha::SessionKey;
using iroha::ibus::DecideKey;
using iroha::ibus::KeyContext;
using iroha::ibus::KeyDecision;
using Kind = KeyDecision::Kind;
namespace m = iroha::ibus;

bool IsSession(const KeyDecision& d, SessionKey key) { return d.kind == Kind::Session && d.key == key; }

KeyContext Idle() { return KeyContext{}; }
KeyContext Composing() {
    KeyContext c;
    c.composing = true;
    return c;
}
KeyContext Segmenting() {
    KeyContext c = Composing();
    c.segmenting = true;
    return c;
}

void TestCharacters() {
    const auto a = DecideKey('a', 0, Idle());
    EXPECT_TRUE(IsSession(a, SessionKey::Character) && a.character == U'a' && !a.shift);
    // 大文字は Shift を押して打ったもの（英字入力の始まり）
    const auto upper = DecideKey('A', m::kShift, Composing());
    EXPECT_TRUE(IsSession(upper, SessionKey::Character) && upper.character == U'A' && upper.shift);
    // 記号・数字・テンキー
    EXPECT_TRUE(DecideKey('-', 0, Idle()).character == U'-');
    EXPECT_TRUE(DecideKey(0xffb3, 0, Idle()).character == U'3');  // KP_3
    EXPECT_TRUE(DecideKey(0xffae, 0, Idle()).character == U'.');  // KP_Decimal
    // 修飾キーだけでは何もしない（合成中でも確定しない）
    EXPECT_TRUE(DecideKey(0xffe1, 0, Composing()).kind == Kind::Pass);  // Shift_L
    EXPECT_TRUE(DecideKey(0xffe3, 0, Composing()).kind == Kind::Pass);  // Control_L
}

void TestEditingKeys() {
    // Enter / Esc / Backspace / 矢印は合成中だけ受ける
    for (uint32_t keyval : {0xff0du, 0xff1bu, 0xff08u, 0xff51u, 0xff52u, 0xff53u, 0xff54u}) {
        EXPECT_TRUE(DecideKey(keyval, 0, Idle()).kind == Kind::Pass);
        EXPECT_TRUE(DecideKey(keyval, 0, Composing()).kind == Kind::Session);
    }
    EXPECT_TRUE(IsSession(DecideKey(0xff0d, 0, Composing()), SessionKey::Enter));
    EXPECT_TRUE(IsSession(DecideKey(0xff8d, 0, Composing()), SessionKey::Enter));  // KP_Enter
    EXPECT_TRUE(IsSession(DecideKey(0xff1b, 0, Composing()), SessionKey::Escape));
    EXPECT_TRUE(IsSession(DecideKey(0xff08, 0, Composing()), SessionKey::Backspace));
    EXPECT_TRUE(IsSession(DecideKey(0xff53, 0, Segmenting()), SessionKey::Right));
    EXPECT_TRUE(IsSession(DecideKey(0xff99, 0, Segmenting()), SessionKey::Down));  // KP_Down
}

void TestSpace() {
    // 入力していないときの Space は、全角の設定で Shift を押していなければ受ける
    EXPECT_TRUE(IsSession(DecideKey(' ', 0, Idle()), SessionKey::Space));
    EXPECT_TRUE(DecideKey(' ', m::kShift, Idle()).kind == Kind::Pass);
    KeyContext halfWidth = Idle();
    halfWidth.idleFullWidthSpace = false;
    EXPECT_TRUE(DecideKey(' ', 0, halfWidth).kind == Kind::Pass);
    const auto shifted = DecideKey(' ', m::kShift, Composing());
    EXPECT_TRUE(IsSession(shifted, SessionKey::Space) && shifted.shift);
}

void TestDisplayForms() {
    // F6〜F10 は入力中だけ。文節変換中は確定してアプリに渡す
    EXPECT_TRUE(IsSession(DecideKey(0xffc3, 0, Composing()), SessionKey::DisplayHiragana));
    EXPECT_TRUE(IsSession(DecideKey(0xffc7, 0, Composing()), SessionKey::DisplayHalfAlphanumeric));
    EXPECT_TRUE(DecideKey(0xffc4, 0, Idle()).kind == Kind::Pass);
    EXPECT_TRUE(DecideKey(0xffc4, 0, Segmenting()).kind == Kind::CommitThenPass);
    // Ctrl+U/I/O/P/T（大文字で来ても同じ）
    EXPECT_TRUE(IsSession(DecideKey('u', m::kControl, Composing()), SessionKey::DisplayHiragana));
    EXPECT_TRUE(IsSession(DecideKey('I', m::kControl | m::kShift, Composing()), SessionKey::DisplayKatakana));
    EXPECT_TRUE(IsSession(DecideKey('t', m::kControl, Composing()), SessionKey::DisplayHalfAlphanumeric));
    // 入力していなければアプリのショートカット
    EXPECT_TRUE(DecideKey('u', m::kControl, Idle()).kind == Kind::Pass);
}

void TestShortcuts() {
    EXPECT_TRUE(IsSession(DecideKey('.', m::kControl, Idle()), SessionKey::TogglePunctuation));
    EXPECT_TRUE(IsSession(DecideKey('p', m::kControl | m::kAlt, Idle()), SessionKey::TogglePrivateMode));
    EXPECT_TRUE(IsSession(DecideKey('P', m::kControl | m::kAlt | m::kShift, Composing()),
                          SessionKey::TogglePrivateMode));
    // 合成中のアプリのショートカット（Ctrl+S・Alt・Super）は確定してから渡す
    EXPECT_TRUE(DecideKey('s', m::kControl, Composing()).kind == Kind::CommitThenPass);
    EXPECT_TRUE(DecideKey('f', m::kAlt, Composing()).kind == Kind::CommitThenPass);
    EXPECT_TRUE(DecideKey('a', m::kSuper, Composing()).kind == Kind::CommitThenPass);
    EXPECT_TRUE(DecideKey('a', m::kMod4, Composing()).kind == Kind::CommitThenPass);
    EXPECT_TRUE(DecideKey('s', m::kControl, Idle()).kind == Kind::Pass);
    // 扱わないキー（Home など）も、合成中なら確定してから渡す
    EXPECT_TRUE(DecideKey(0xff50, 0, Composing()).kind == Kind::CommitThenPass);
    EXPECT_TRUE(DecideKey(0xff50, 0, Idle()).kind == Kind::Pass);
}

void TestTabAndUndo() {
    KeyContext predicting = Composing();
    predicting.hasPredictions = true;
    EXPECT_TRUE(IsSession(DecideKey(0xff09, 0, predicting), SessionKey::Tab));
    const auto back = DecideKey(0xfe20, m::kShift, predicting);  // ISO_Left_Tab
    EXPECT_TRUE(IsSession(back, SessionKey::Tab) && back.shift);
    EXPECT_TRUE(DecideKey(0xff09, 0, Composing()).kind == Kind::CommitThenPass);
    EXPECT_TRUE(DecideKey(0xff09, 0, Idle()).kind == Kind::Pass);
    // Ctrl+Backspace は、合成していなくて取り消せる確定があるときだけ
    KeyContext undoable = Idle();
    undoable.idleUndoable = true;
    EXPECT_TRUE(DecideKey(0xff08, m::kControl, undoable).kind == Kind::UndoCommit);
    EXPECT_TRUE(DecideKey(0xff08, m::kControl | m::kShift, undoable).kind == Kind::Pass);
    EXPECT_TRUE(DecideKey(0xff08, m::kControl, Idle()).kind == Kind::Pass);
}

void TestInputModes() {
    EXPECT_TRUE(DecideKey(0xff2a, 0, Idle()).kind == Kind::ToggleDirectMode);  // Zenkaku_Hankaku
    EXPECT_TRUE(DecideKey(0xff21, 0, Idle()).kind == Kind::ToggleDirectMode);  // Kanji
    EXPECT_TRUE(DecideKey('`', m::kAlt, Idle()).kind == Kind::ToggleDirectMode);
    EXPECT_TRUE(DecideKey(0xff27, 0, Idle()).kind == Kind::SetKanaMode);      // Hiragana_Katakana
    EXPECT_TRUE(DecideKey(0xff30, 0, Idle()).kind == Kind::SetDirectMode);    // Eisu_toggle
    // 英数モード中は切り替えキー以外を食わない
    KeyContext direct = Idle();
    direct.directMode = true;
    EXPECT_TRUE(DecideKey('a', 0, direct).kind == Kind::Pass);
    EXPECT_TRUE(DecideKey(' ', 0, direct).kind == Kind::Pass);
    EXPECT_TRUE(DecideKey(0xff2a, 0, direct).kind == Kind::ToggleDirectMode);
}

} // namespace

int main() {
    TestCharacters();
    TestEditingKeys();
    TestSpace();
    TestDisplayForms();
    TestShortcuts();
    TestTabAndUndo();
    TestInputModes();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
