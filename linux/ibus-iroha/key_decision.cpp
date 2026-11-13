#include "key_decision.h"

namespace iroha::ibus {

namespace {

// X の keysym（ibuskeysyms.h と同じ値。テストで IBus のヘッダを使わずに済むようにここで持つ）
namespace key {
constexpr uint32_t BackSpace = 0xff08;
constexpr uint32_t Tab = 0xff09;
constexpr uint32_t Return = 0xff0d;
constexpr uint32_t Escape = 0xff1b;
constexpr uint32_t Kanji = 0xff21;
constexpr uint32_t Hiragana = 0xff25;
constexpr uint32_t Katakana = 0xff26;
constexpr uint32_t Hiragana_Katakana = 0xff27;
constexpr uint32_t Zenkaku_Hankaku = 0xff2a;
constexpr uint32_t Eisu_toggle = 0xff30;
constexpr uint32_t Left = 0xff51;
constexpr uint32_t Up = 0xff52;
constexpr uint32_t Right = 0xff53;
constexpr uint32_t Down = 0xff54;
constexpr uint32_t KP_Space = 0xff80;
constexpr uint32_t KP_Tab = 0xff89;
constexpr uint32_t KP_Enter = 0xff8d;
constexpr uint32_t KP_Left = 0xff96;
constexpr uint32_t KP_Up = 0xff97;
constexpr uint32_t KP_Right = 0xff98;
constexpr uint32_t KP_Down = 0xff99;
constexpr uint32_t F6 = 0xffc3;
constexpr uint32_t F7 = 0xffc4;
constexpr uint32_t F8 = 0xffc5;
constexpr uint32_t F9 = 0xffc6;
constexpr uint32_t F10 = 0xffc7;
constexpr uint32_t ISO_Left_Tab = 0xfe20;
constexpr uint32_t space = 0x20;
constexpr uint32_t grave = 0x60;
} // namespace key

// 修飾キー・ロックキー（単独では何もしない。合成を確定させない）
bool IsModifierKey(uint32_t keyval) {
    if (keyval >= 0xffe1 && keyval <= 0xffee) return true; // Shift_L … Hyper_R（Caps_Lock を含む）
    if (keyval >= 0xfe01 && keyval <= 0xfe13) return true; // ISO_Lock … ISO_Level5_Lock
    switch (keyval) {
        case 0xff7e: // Mode_switch
        case 0xff7f: // Num_Lock
        case 0xff14: // Scroll_Lock
        case 0xff67: // Menu
            return true;
        default:
            return false;
    }
}

char32_t Lower(char32_t c) { return c >= U'A' && c <= U'Z' ? c - U'A' + U'a' : c; }

} // namespace

char32_t PrintableAscii(uint32_t keyval) {
    if (keyval >= 0x21 && keyval <= 0x7e) return static_cast<char32_t>(keyval);
    if (keyval >= 0xffb0 && keyval <= 0xffb9) return static_cast<char32_t>(U'0' + (keyval - 0xffb0)); // KP_0…KP_9
    switch (keyval) {
        case 0xffaa: return U'*'; // KP_Multiply
        case 0xffab: return U'+'; // KP_Add
        case 0xffac: return U','; // KP_Separator
        case 0xffad: return U'-'; // KP_Subtract
        case 0xffae: return U'.'; // KP_Decimal
        case 0xffaf: return U'/'; // KP_Divide
        case 0xffbd: return U'='; // KP_Equal
        default: return 0;
    }
}

KeyDecision DecideKey(uint32_t keyval, uint32_t modifiers, const KeyContext& context) {
    KeyDecision decision;
    const bool alt = (modifiers & kAlt) != 0;
    // 入力モードの切り替えキーは英数モード中でも受ける。
    //   半角/全角（Zenkaku_Hankaku・Kanji、US配列は Alt+`）、ひらがなキー、英数キー
    switch (keyval) {
        case key::Zenkaku_Hankaku:
        case key::Kanji:
            decision.kind = KeyDecision::Kind::ToggleDirectMode;
            return decision;
        case key::Hiragana_Katakana:
        case key::Hiragana:
        case key::Katakana:
            decision.kind = KeyDecision::Kind::SetKanaMode;
            return decision;
        case key::Eisu_toggle:
            decision.kind = KeyDecision::Kind::SetDirectMode;
            return decision;
        case key::grave:
            if (alt) {
                decision.kind = KeyDecision::Kind::ToggleDirectMode;
                return decision;
            }
            break;
        default:
            break;
    }
    // 英数モード中はキーを一切食わない
    if (context.directMode) return decision;

    const bool composing = context.composing;
    const bool segmenting = composing && context.segmenting;
    const bool control = (modifiers & kControl) != 0;
    const bool super = (modifiers & (kSuper | kMod4)) != 0;
    decision.shift = (modifiers & kShift) != 0;
    const auto session = [&decision](SessionKey sessionKey) {
        decision.kind = KeyDecision::Kind::Session;
        decision.key = sessionKey;
        return decision;
    };
    const auto commitThenPass = [&decision, composing, keyval] {
        if (composing && !IsModifierKey(keyval)) decision.kind = KeyDecision::Kind::CommitThenPass;
        return decision;
    };
    const char32_t printable = PrintableAscii(keyval);
    // プライベートモードの切り替え（Ctrl+Alt+P。入力中かどうかによらず効く。macOS版の Ctrl+Option+P）
    if (control && alt && Lower(printable) == U'p') return session(SessionKey::TogglePrivateMode);
    if (alt || super) return commitThenPass();
    if (control) {
        if (keyval == U'.') return session(SessionKey::TogglePunctuation);
        if (composing && !segmenting) {
            switch (Lower(printable)) {
                case U'u': return session(SessionKey::DisplayHiragana);
                case U'i': return session(SessionKey::DisplayKatakana);
                case U'o': return session(SessionKey::DisplayHalfKatakana);
                case U'p': return session(SessionKey::DisplayFullAlphanumeric);
                case U't': return session(SessionKey::DisplayHalfAlphanumeric);
                default: break;
            }
        }
        if (keyval == key::BackSpace && !composing && context.idleUndoable && !decision.shift) {
            decision.kind = KeyDecision::Kind::UndoCommit;
            return decision;
        }
        return commitThenPass();
    }
    switch (keyval) {
        case key::F6: case key::F7: case key::F8: case key::F9: case key::F10:
            if (!composing) return decision;
            if (segmenting) return commitThenPass();
            return session(keyval == key::F6   ? SessionKey::DisplayHiragana
                           : keyval == key::F7 ? SessionKey::DisplayKatakana
                           : keyval == key::F8 ? SessionKey::DisplayHalfKatakana
                           : keyval == key::F9 ? SessionKey::DisplayFullAlphanumeric
                                               : SessionKey::DisplayHalfAlphanumeric);
        case key::Tab:
        case key::KP_Tab:
        case key::ISO_Left_Tab:
            // 予測変換の候補が出ていれば Tab で選ぶ（Shift+Tab は前の候補）
            if (composing && !segmenting && context.hasPredictions) return session(SessionKey::Tab);
            return commitThenPass();
        case key::Return:
        case key::KP_Enter:
            return composing ? session(SessionKey::Enter) : decision;
        case key::Escape:
            return composing ? session(SessionKey::Escape) : decision;
        case key::BackSpace:
            return composing ? session(SessionKey::Backspace) : decision;
        case key::space:
        case key::KP_Space:
            // 入力していないときの Space は、設定で全角にしていて Shift を押していなければ受ける
            if (composing || (context.idleFullWidthSpace && !decision.shift)) return session(SessionKey::Space);
            return decision;
        case key::Down:
        case key::KP_Down:
            return composing ? session(SessionKey::Down) : decision;
        case key::Up:
        case key::KP_Up:
            return composing ? session(SessionKey::Up) : decision;
        case key::Left:
        case key::KP_Left:
            return composing ? session(SessionKey::Left) : decision;
        case key::Right:
        case key::KP_Right:
            return composing ? session(SessionKey::Right) : decision;
        default:
            break;
    }
    if (printable != 0) {
        decision.character = printable;
        return session(SessionKey::Character);
    }
    return commitThenPass();
}

} // namespace iroha::ibus
