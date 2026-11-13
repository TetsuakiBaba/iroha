#pragma once
// キー（IBus の keyval と修飾キーの状態）を意味に読み替え、受けるかどうかを決める。
// IBus に依存しない純粋な関数にしてあり、単体テストできる（tests/test_key_decision.cpp）。
// 規則は Windows版 TIP の TextService::DecideKey（macOS版 IrohaInputController.handleKey と同じ扱い）:
//   印字できる ASCII 文字は常に、Enter / Esc / Backspace / Space・矢印は合成中だけ
//   （矢印は文節変換中は文節の移動、入力中は握りつぶす）。入力していないときの Space は設定で全角にする。
//   F6〜F10・Ctrl+U/I/O/P/T は入力中（文節変換中でない）だけ、Ctrl+. は常に受ける。
//   合成中に IME が扱わないキー（Ctrl+S・Alt・Super の組み合わせなど）は、確定してからアプリに渡す
#include <cstdint>

#include "iroha/input_session.h"

namespace iroha::ibus {

// 修飾キーの状態（IBus の IBusModifierType のうち使うもの。値も同じ）
enum Modifier : uint32_t {
    kShift = 1 << 0,
    kControl = 1 << 2,
    kAlt = 1 << 3,       // IBUS_MOD1_MASK
    kSuper = 1 << 26,    // IBUS_SUPER_MASK
    kMod4 = 1 << 6,      // IBUS_MOD4_MASK（Super として扱う）
    kRelease = 1 << 30,  // IBUS_RELEASE_MASK
};

// キーを決めるのに使う、直近の表示の状態
struct KeyContext {
    bool directMode = false;      // 英数モード（キーを食わない）
    bool composing = false;       // 未確定文字列がある
    bool segmenting = false;      // 文節変換中
    bool hasPredictions = false;  // 予測変換の候補が出ている
    bool idleFullWidthSpace = true;  // 入力していないときの Space を全角にする（設定）
    bool idleUndoable = false;    // 直前の確定を取り消せる（Ctrl+Backspace）
};

struct KeyDecision {
    enum class Kind {
        Pass,             // 受けない（アプリに渡す。合成はそのまま）
        Session,          // セッションに key を送る
        CommitThenPass,   // 今の表示のまま確定してからアプリに渡す
        UndoCommit,       // 直前の確定の取り消しを試みる（カーソルの手前を確かめてから）
        ToggleDirectMode, // かな ⇄ 英数
        SetKanaMode,
        SetDirectMode,
    };
    Kind kind = Kind::Pass;
    SessionKey key = SessionKey::Character;
    char32_t character = 0;
    bool shift = false;
};

// 押したキーの読み替え（離したキーは呼ぶ側で除く）
KeyDecision DecideKey(uint32_t keyval, uint32_t modifiers, const KeyContext& context);

// keyval を印字できる ASCII 文字にする（テンキーを含む）。当たらなければ 0
char32_t PrintableAscii(uint32_t keyval);

} // namespace iroha::ibus
