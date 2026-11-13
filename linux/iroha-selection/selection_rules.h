#pragma once
// 選択テキストのAI編集のうち、画面と X に頼らない規則（単体テストする）。
// 設定の読み方・使わないアプリの照合・ショートカットから X のキーと修飾キーへの変換・AI への依頼の組み立て。
// 移植元: windows/iroha-server/selection_assistant.cpp と main.cpp の LoadSelection（コミット c12af85）

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "iroha/ai_conversion.h"
#include "iroha/config.h"
#include "key_shortcut.h"

namespace iroha::selection {

// その場で指示するキーの番号（プリセットの番号 0〜4 と重ならない値。Windows版と同じ）
constexpr int kOnDemandId = 100;

struct Settings {
    bool enabled = false;
    struct Preset {
        std::u32string name;
        std::u32string prompt; // 空なら既定のプロンプト
        std::string hotkey;    // "Ctrl+Alt+1" の形（空なら割り当てなし）
        bool enabled = true;
    };
    std::vector<Preset> presets;
    std::string onDemandHotkey;            // その場で指示するキー（空なら割り当てなし）
    std::vector<std::string> excludedApps; // 使わないアプリの名前（小文字）
    // マウスで選んだときの出し方（macOS版の SelectionTriggerMode。config の selectionTriggerMode。
    // AltMenu は macOS版の optionMenu（⌥ の代わりに Alt））
    enum class Trigger { Off, Bubble, ImmediateMenu, AltMenu };
    Trigger trigger = Trigger::Bubble;
    // 選んだ文字数をマウスの近くに出す（AI編集が OFF でも、文字数だけを出す）
    bool characterCount = false;

    // マウスで選んだときに AI編集のアイコン・メニューを出すか（alt: 選び終えたときに Alt を押していたか）
    bool MouseTriggersAI(bool alt) const {
        return enabled && trigger != Trigger::Off && (trigger != Trigger::AltMenu || alt);
    }
    // マウスの操作を見る必要があるか（AI編集のマウスのきっかけか、文字数の表示を使うとき）
    bool WatchesMouse() const { return (enabled && trigger != Trigger::Off) || characterCount; }
};
Settings::Trigger ParseTrigger(const std::string& value);

// config.json の selection* から作る（書いていない項目は SelectionSettingsDefaults）
Settings SettingsFromConfig(const Config& config);

// 使わないアプリの一覧（カンマか改行で区切った名前）を、前後の空白を除いて小文字にする
std::vector<std::string> ParseExcludedApps(const std::string& text);
// ASCII の英字だけを小文字にする（実行ファイルの名前・WM_CLASS の比較用）
std::string Lower(std::string text);

// 前面の窓のアプリを表す名前（/proc/<pid>/comm と WM_CLASS の 2 つ。取れなかったものは空）
struct AppIdentity {
    std::string command;       // /proc/<pid>/comm（実行ファイルの名前。15 バイトで切れている）
    std::string classInstance; // WM_CLASS の 1 つ目（例 "gedit"）
    std::string className;     // WM_CLASS の 2 つ目（例 "Gedit"）
};
// 使わないアプリか。iroha 自身（名前が "iroha" で始まる）も使わない。名前は小文字で比べる
bool IsExcluded(const std::vector<std::string>& excludedApps, const AppIdentity& app);

// X の修飾キーの値（<X11/X.h> の ShiftMask・LockMask・ControlMask・Mod1Mask と同じ）
constexpr unsigned kShiftMask = 1u << 0;
constexpr unsigned kLockMask = 1u << 1;
constexpr unsigned kControlMask = 1u << 2;
constexpr unsigned kAltMask = 1u << 3; // Mod1（Alt）

// つかむキー 1 つ分（keysym と、ショートカットの修飾キー）
struct HotkeyBinding {
    int id = 0;            // プリセットの番号か kOnDemandId
    std::string text;      // 設定に書かれた文字列（記録用）
    uint32_t keysym = 0;
    unsigned modifiers = 0; // kShiftMask・kControlMask・kAltMask の組
};

// ショートカットの修飾キーを X の修飾キーの値にする
unsigned ModifierMask(const KeyShortcut& shortcut);
// 有効なプリセットのキーとその場で指示するキーを、つかむ一覧にする。読めない文字列は飛ばし、
// 同じキーが複数の枠に入っていたら先勝ち（二重に動かない。Windows版と同じ）
std::vector<HotkeyBinding> PlanHotkeys(const Settings& settings);
// NumLock・CapsLock の状態に関係なく受けるため、つかむときに足す修飾キーの組（numLockMask が 0 なら CapsLock だけ）
std::vector<unsigned> LockVariants(unsigned modifiers, unsigned numLockMask);

// つかんだキー（keysym を X のキーの番号にしたもの）
struct GrabbedKey {
    int id = 0;
    unsigned keycode = 0;
    unsigned modifiers = 0;
};
// 届いたキー（キーの番号と修飾キーの状態）がどのショートカットか。NumLock・CapsLock は見ない。当たらなければ -1
int FindHotkey(const std::vector<GrabbedKey>& keys, unsigned keycode, unsigned state, unsigned numLockMask);

// プリセットの見出し（名前が空なら「プリセットN」）
std::u32string PresetHeading(const Settings::Preset& preset, size_t index);
// プリセットで依頼する（プロンプトが空なら既定のプロンプト）
AIRequest PresetRequest(const Settings::Preset& preset, size_t index, const std::u32string& source);
// その場で入れた指示で依頼する（選択が無ければ文章を作る）
AIRequest PromptRequest(const std::u32string& instruction, const std::u32string& source, bool generation);
// 結果の窓に出す選択の 1 行（改行を空白にする）
std::u32string SourceLine(const std::u32string& source, bool generation);
// PRIMARY 選択から取れた文字列を選択として使うか（空白だけなら選択なしとみなす）
std::optional<std::u32string> SelectionFromPrimary(const std::optional<std::string>& utf8);

// マウスの左ボタンの押し離しから「文章を選んだらしい操作」（ドラッグかダブルクリック）を見分ける
// （macOS版 MouseSelectionMonitor・Windows版のマウスのフックと同じ規則）。座標は画面のピクセル、時刻はミリ秒
class GestureTracker {
public:
    // dragDistance: これ以上動かしたらドラッグ。doubleClickTime・doubleClickDistance: ダブルクリックとみなす間隔と距離
    GestureTracker(int dragDistance, unsigned long doubleClickTime, int doubleClickDistance)
        : dragDistance_(dragDistance), doubleClickTime_(doubleClickTime), doubleClickDistance_(doubleClickDistance) {}
    void Press(int x, int y);
    // 離した。選んだらしい操作なら真
    bool Release(int x, int y, unsigned long time);

private:
    int dragDistance_;
    unsigned long doubleClickTime_;
    int doubleClickDistance_;
    bool down_ = false;
    int downX_ = 0;
    int downY_ = 0;
    bool hasLastUp_ = false;
    unsigned long lastUpTime_ = 0;
    int lastUpX_ = 0;
    int lastUpY_ = 0;
};

// マウスで選んだあとに読んだ PRIMARY 選択を、アイコン・文字数に使うか。
// 押してから選択の持ち主が変わっていれば使う（変わっていなければ、窓を動かしただけなどで、前からある選択）。
// 持ち主の変化を見られないとき（XFixes が無い）は、前に使った文章と違えば使う
bool ShouldUseMouseSelection(const std::optional<std::u32string>& text, bool canWatchOwner, bool ownerChanged,
                             const std::u32string& previous);

} // namespace iroha::selection
