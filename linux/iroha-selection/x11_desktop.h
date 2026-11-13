#pragma once
// 選択テキストのAI編集が X に頼む部分（GTK とは別に開いた X の接続で行う）。
//   ルートの窓にショートカットをつかむ（XGrabKey）・前面の窓とそのアプリを調べる・窓を前面に出す・
//   キーが離されたかを調べる・Ctrl+V を送る（XTest）・マウスポインタの位置・結果の窓を動かす・
//   マウスの左ボタンの押し離しを見る（XInput2 の生のイベント。つかまないので、ほかのアプリの操作は変わらない）・
//   PRIMARY 選択の持ち主が変わったことを見る（XFixes）。
// main thread だけで使う（X の接続は 1 本で、届いたキーは GLib の main loop で受ける）

// selection_rules.h（iroha のヘッダ）を X のヘッダより先に読む（X は None などをマクロにしている）
#include "selection_rules.h"

#include <X11/Xlib.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace iroha::selection {

class X11Desktop {
public:
    using Logger = std::function<void(const std::string&)>;
    // つかんだキーが押された（番号と X のイベントの時刻）
    using HotkeyHandler = std::function<void(int id, unsigned long time)>;
    // マウスの左ボタンを押した・離した（位置はルートの窓の座標、time は X の時刻（ミリ秒）、alt は Alt を押していたか）
    struct MouseEvent {
        bool press = false;
        int x = 0;
        int y = 0;
        unsigned long time = 0;
        bool alt = false;
    };
    using MouseHandler = std::function<void(const MouseEvent&)>;

    X11Desktop(Logger log, HotkeyHandler onHotkey);
    ~X11Desktop();
    X11Desktop(const X11Desktop&) = delete;
    X11Desktop& operator=(const X11Desktop&) = delete;

    // X の接続を開き、届いたイベントを main loop で読むようにする。開けなければ偽
    bool Open();

    // ショートカットをつかみ直す（前につかんだものは放す）。つかめた数を返す
    size_t Grab(const std::vector<HotkeyBinding>& bindings);
    void Ungrab();

    // マウスの左ボタンを見る・見ない（XInput2 が無ければ偽）
    bool WatchMouse(bool enabled, MouseHandler handler);
    // PRIMARY 選択の持ち主が変わった回数（XFixes が無ければ常に 0。CanWatchSelectionOwner で確かめる）
    uint64_t PrimaryOwnerChanges() const { return primaryChanges_; }
    bool CanWatchSelectionOwner() const { return xfixes_; }
    // 直近にマウスのボタンを離した X の時刻（メニューを選んだクリックの時刻として、結果の窓を出すときに使う）
    unsigned long LastButtonTime() const { return lastButtonTime_; }

    // 前面の窓（_NET_ACTIVE_WINDOW）。無ければ 0
    Window ActiveWindow();
    // マウスポインタの下にあるトップレベルの窓（ウィンドウマネージャが無いときの前面の窓の代わり）。無ければ 0
    Window WindowAtPointer();
    // 窓のアプリ（_NET_WM_PID → /proc/<pid>/comm と WM_CLASS）
    AppIdentity Identify(Window window);
    // 窓を前面に出すようウィンドウマネージャに頼む（_NET_ACTIVE_WINDOW のクライアントメッセージ。time は 0 でよい）
    void Activate(Window window, unsigned long time);
    // 窓がまだあるか
    bool Exists(Window window);
    // Ctrl・Alt・Shift・Super・Enter のどれかが押されているか（押したまま Ctrl+V を送ると Ctrl+Alt+V などになる。
    // Enter は結果の窓で押したもので、押し続けると自動の繰り返しが元のアプリに届く）
    bool KeysHeld();
    // Ctrl+V を送る（XTest）。送れなければ偽
    bool SendPaste();
    // マウスポインタの位置（ルートの窓の座標。取れなければ無し）
    std::optional<std::pair<int, int>> PointerPosition();
    // 窓を (x, y) に動かし、最前面・タスクバーに出さない指定を頼む（結果の窓に使う）
    void PlaceWindow(Window window, int x, int y);
    void KeepAbove(Window window);
    // ウィンドウマネージャに扱わせない窓にする（アイコン・メニュー。押してもフォーカスが移らない。出す前に呼ぶ）
    void MakeOverlay(Window window);
    // 窓の位置と大きさ（ルートの窓の座標）。取れなければ無し
    struct Rect {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        bool Contains(int px, int py) const { return px >= x && px < x + width && py >= y && py < y + height; }
    };
    std::optional<Rect> WindowRect(Window window);

private:
    struct EventSource;
    static int HandleError(Display* display, XErrorEvent* error);
    void Dispatch();
    unsigned NumLockMask();
    std::optional<unsigned long> CardinalProperty(Window window, Atom property);
    void SendClientMessage(Window window, Atom type, long l0, long l1, long l2, long l3);

    Logger log_;
    HotkeyHandler onHotkey_;
    Display* display_ = nullptr;
    Window root_ = 0;
    unsigned numLockMask_ = 0;
    std::vector<GrabbedKey> grabbed_;
    EventSource* source_ = nullptr;
    bool xtest_ = false;
    // XInput2（マウスのボタン）と XFixes（選択の持ち主）
    int xiOpcode_ = -1;
    bool mouseWatched_ = false;
    MouseHandler onMouse_;
    bool xfixes_ = false;
    int xfixesEventBase_ = 0;
    uint64_t primaryChanges_ = 0;
    unsigned long lastButtonTime_ = 0;
    void DispatchGeneric(XEvent& event);
};

} // namespace iroha::selection
