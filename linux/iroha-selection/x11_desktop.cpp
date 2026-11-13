#include "x11_desktop.h"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>
#include <X11/keysym.h>
#include <glib.h>

#include <fstream>

namespace iroha::selection {

namespace {

// X のエラーは非同期に届き、既定の処理はプロセスを終わらせる（GDK の処理も、知らない接続のエラーで止まる）。
// この接続のエラーは記録だけにして、ほかの接続のエラーは前の処理に回す
Display* g_display = nullptr;
XErrorHandler g_previousHandler = nullptr;
int g_lastError = 0;

// 直前までの要求を送り切り、そのあいだに起きたエラーの番号を返す（0 ならエラーなし）
int SyncAndTakeError(Display* display) {
    XSync(display, False);
    const int error = g_lastError;
    g_lastError = 0;
    return error;
}

} // namespace

int X11Desktop::HandleError(Display* display, XErrorEvent* error) {
    if (display == g_display) {
        g_lastError = error->error_code;
        return 0;
    }
    return g_previousHandler ? g_previousHandler(display, error) : 0;
}

// X の接続に届いたイベントを GLib の main loop で読む
struct X11Desktop::EventSource {
    GSource source;
    GPollFD poll;
    X11Desktop* owner;
};

X11Desktop::X11Desktop(Logger log, HotkeyHandler onHotkey) : log_(std::move(log)), onHotkey_(std::move(onHotkey)) {}

X11Desktop::~X11Desktop() {
    if (source_) {
        g_source_destroy(&source_->source);
        g_source_unref(&source_->source);
        source_ = nullptr;
    }
    if (display_) {
        Ungrab();
        XCloseDisplay(display_);
        if (g_display == display_) g_display = nullptr;
        display_ = nullptr;
    }
}

bool X11Desktop::Open() {
    display_ = XOpenDisplay(nullptr);
    if (!display_) return false;
    root_ = DefaultRootWindow(display_);
    g_display = display_;
    g_previousHandler = XSetErrorHandler(&X11Desktop::HandleError);
    int event = 0;
    int error = 0;
    int major = 0;
    int minor = 0;
    xtest_ = XTestQueryExtension(display_, &event, &error, &major, &minor) != 0;
    if (!xtest_) log_("XTest is not available (results can be copied but not pasted)");
    // XInput2: マウスのボタンの生のイベントをルートの窓で受ける。2.1 以上と名乗る（2.0 のままだと、ボタンを押した
    // アプリが持つ暗黙のつかみの間（押してから離すまで）の生のイベントが届かず、離したことが分からない）
    int xiEvent = 0;
    int xiError = 0;
    if (XQueryExtension(display_, "XInputExtension", &xiOpcode_, &xiEvent, &xiError)) {
        int xiMajor = 2;
        int xiMinor = 2;
        if (XIQueryVersion(display_, &xiMajor, &xiMinor) != Success || (xiMajor == 2 && xiMinor < 1)) xiOpcode_ = -1;
    } else {
        xiOpcode_ = -1;
    }
    if (xiOpcode_ < 0) log_("XInput2 is not available (selecting with the mouse is not watched)");
    // XFixes: PRIMARY 選択の持ち主が変わったら知らせてもらう
    int fixesError = 0;
    if (XFixesQueryExtension(display_, &xfixesEventBase_, &fixesError)) {
        xfixes_ = true;
        XFixesSelectSelectionInput(display_, root_, XA_PRIMARY, XFixesSetSelectionOwnerNotifyMask);
    } else {
        log_("XFixes is not available (the selection is compared with the previous one instead)");
    }

    static GSourceFuncs funcs = {
        // prepare: 既に読み込んであるイベントがあればすぐ配る
        [](GSource* source, gint* timeout) -> gboolean {
            *timeout = -1;
            auto* self = reinterpret_cast<EventSource*>(source);
            return XPending(self->owner->display_) > 0;
        },
        // check: 接続に読めるものが届いたか
        [](GSource* source) -> gboolean {
            auto* self = reinterpret_cast<EventSource*>(source);
            if ((self->poll.revents & G_IO_IN) == 0) return FALSE;
            return XPending(self->owner->display_) > 0;
        },
        [](GSource* source, GSourceFunc, gpointer) -> gboolean {
            reinterpret_cast<EventSource*>(source)->owner->Dispatch();
            return G_SOURCE_CONTINUE;
        },
        nullptr,
        nullptr,
        nullptr,
    };
    source_ = reinterpret_cast<EventSource*>(g_source_new(&funcs, sizeof(EventSource)));
    source_->owner = this;
    source_->poll.fd = ConnectionNumber(display_);
    source_->poll.events = G_IO_IN;
    source_->poll.revents = 0;
    g_source_add_poll(&source_->source, &source_->poll);
    g_source_set_name(&source_->source, "iroha-selection X11");
    g_source_attach(&source_->source, nullptr);
    return true;
}

void X11Desktop::Dispatch() {
    while (display_ && XPending(display_) > 0) {
        XEvent event;
        XNextEvent(display_, &event);
        if (xfixes_ && event.type == xfixesEventBase_ + XFixesSelectionNotify) {
            ++primaryChanges_;
            continue;
        }
        if (event.type == GenericEvent) {
            DispatchGeneric(event);
            continue;
        }
        if (event.type != KeyPress) continue;
        const int id = FindHotkey(grabbed_, event.xkey.keycode, event.xkey.state, numLockMask_);
        if (id >= 0 && onHotkey_) onHotkey_(id, event.xkey.time);
    }
}

void X11Desktop::DispatchGeneric(XEvent& event) {
    if (event.xcookie.extension != xiOpcode_ || !XGetEventData(display_, &event.xcookie)) return;
    const int type = event.xcookie.evtype;
    if (type == XI_RawButtonPress || type == XI_RawButtonRelease) {
        const auto* raw = static_cast<const XIRawEvent*>(event.xcookie.data);
        if (raw->detail == 1 && mouseWatched_ && onMouse_) {
            // 生のイベントには位置が無いので、ポインタの位置と修飾キーの状態をここで読む
            MouseEvent mouse;
            mouse.press = type == XI_RawButtonPress;
            mouse.time = raw->time;
            Window rootReturn = 0;
            Window child = 0;
            int windowX = 0;
            int windowY = 0;
            unsigned mask = 0;
            if (XQueryPointer(display_, root_, &rootReturn, &child, &mouse.x, &mouse.y, &windowX, &windowY, &mask)) {
                mouse.alt = (mask & Mod1Mask) != 0;
            }
            if (!mouse.press) lastButtonTime_ = raw->time;
            XFreeEventData(display_, &event.xcookie);
            onMouse_(mouse);
            return;
        }
        if (type == XI_RawButtonRelease) lastButtonTime_ = raw->time;
    }
    XFreeEventData(display_, &event.xcookie);
}

bool X11Desktop::WatchMouse(bool enabled, MouseHandler handler) {
    if (!display_ || xiOpcode_ < 0) return false;
    onMouse_ = std::move(handler);
    if (enabled == mouseWatched_) return true;
    unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {};
    if (enabled) {
        XISetMask(bits, XI_RawButtonPress);
        XISetMask(bits, XI_RawButtonRelease);
    }
    XIEventMask mask;
    mask.deviceid = XIAllMasterDevices;
    mask.mask_len = sizeof(bits);
    mask.mask = bits;
    XISelectEvents(display_, root_, &mask, 1);
    const int error = SyncAndTakeError(display_);
    if (error != 0) {
        log_("cannot watch the mouse (X error " + std::to_string(error) + ")");
        return false;
    }
    mouseWatched_ = enabled;
    log_(enabled ? "selection: watching the mouse" : "selection: not watching the mouse");
    return true;
}

unsigned X11Desktop::NumLockMask() {
    unsigned mask = 0;
    XModifierKeymap* map = XGetModifierMapping(display_);
    if (!map) return 0;
    const KeyCode numLock = XKeysymToKeycode(display_, XK_Num_Lock);
    for (int modifier = 0; modifier < 8 && numLock != 0; ++modifier) {
        for (int i = 0; i < map->max_keypermod; ++i) {
            if (map->modifiermap[modifier * map->max_keypermod + i] == numLock) mask = 1u << modifier;
        }
    }
    XFreeModifiermap(map);
    return mask;
}

size_t X11Desktop::Grab(const std::vector<HotkeyBinding>& bindings) {
    if (!display_) return 0;
    Ungrab();
    numLockMask_ = NumLockMask();
    size_t count = 0;
    for (const HotkeyBinding& binding : bindings) {
        const KeyCode keycode = XKeysymToKeycode(display_, binding.keysym);
        if (keycode == 0) {
            log_("hotkey " + binding.text + " has no key on this keyboard");
            continue;
        }
        const std::vector<unsigned> variants = LockVariants(binding.modifiers, numLockMask_);
        SyncAndTakeError(display_);
        for (unsigned modifiers : variants) {
            XGrabKey(display_, keycode, modifiers, root_, False, GrabModeAsync, GrabModeAsync);
        }
        const int error = SyncAndTakeError(display_);
        if (error != 0) {
            // ほかのアプリがつかんでいる（BadAccess）。一部だけつかめていたら放す
            for (unsigned modifiers : variants) XUngrabKey(display_, keycode, modifiers, root_);
            SyncAndTakeError(display_);
            log_("hotkey " + binding.text + " is already in use (X error " + std::to_string(error) + ")");
            continue;
        }
        grabbed_.push_back({binding.id, keycode, binding.modifiers});
        ++count;
    }
    return count;
}

void X11Desktop::Ungrab() {
    if (!display_) return;
    for (const GrabbedKey& key : grabbed_) {
        for (unsigned modifiers : LockVariants(key.modifiers, numLockMask_)) {
            XUngrabKey(display_, key.keycode, modifiers, root_);
        }
    }
    grabbed_.clear();
    SyncAndTakeError(display_);
}

std::optional<unsigned long> X11Desktop::CardinalProperty(Window window, Atom property) {
    Atom type = 0;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    const int status = XGetWindowProperty(display_, window, property, 0, 1, False, AnyPropertyType, &type, &format,
                                          &count, &remaining, &data);
    SyncAndTakeError(display_);
    std::optional<unsigned long> value;
    if (status == Success && data && count == 1 && format == 32) {
        // 32 ビットの値は long の並びで返る
        value = static_cast<unsigned long>(reinterpret_cast<long*>(data)[0]);
    }
    if (data) XFree(data);
    return value;
}

Window X11Desktop::ActiveWindow() {
    if (!display_) return 0;
    const Atom active = XInternAtom(display_, "_NET_ACTIVE_WINDOW", False);
    const auto window = CardinalProperty(root_, active);
    return window ? static_cast<Window>(*window) : 0;
}

Window X11Desktop::WindowAtPointer() {
    if (!display_) return 0;
    Window rootReturn = 0;
    Window child = 0;
    int rootX = 0;
    int rootY = 0;
    int windowX = 0;
    int windowY = 0;
    unsigned mask = 0;
    if (!XQueryPointer(display_, root_, &rootReturn, &child, &rootX, &rootY, &windowX, &windowY, &mask)) return 0;
    return child;
}

AppIdentity X11Desktop::Identify(Window window) {
    AppIdentity app;
    if (!display_ || window == 0) return app;
    const Atom pidAtom = XInternAtom(display_, "_NET_WM_PID", False);
    if (const auto pid = CardinalProperty(window, pidAtom); pid && *pid > 0) {
        std::ifstream file("/proc/" + std::to_string(*pid) + "/comm");
        std::getline(file, app.command);
    }
    XClassHint hint = {nullptr, nullptr};
    if (XGetClassHint(display_, window, &hint)) {
        if (hint.res_name) app.classInstance = hint.res_name;
        if (hint.res_class) app.className = hint.res_class;
    }
    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
    SyncAndTakeError(display_);
    return app;
}

void X11Desktop::SendClientMessage(Window window, Atom type, long l0, long l1, long l2, long l3) {
    XEvent event = {};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = type;
    event.xclient.format = 32;
    event.xclient.data.l[0] = l0;
    event.xclient.data.l[1] = l1;
    event.xclient.data.l[2] = l2;
    event.xclient.data.l[3] = l3;
    XSendEvent(display_, root_, False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
    SyncAndTakeError(display_);
}

void X11Desktop::Activate(Window window, unsigned long time) {
    if (!display_ || window == 0) return;
    // 依頼元は 2（ページャなど、利用者の操作を代わりに伝えるもの）にする。1（普通のアプリ）だと、
    // ウィンドウマネージャがフォーカスを奪う動きとみなして断ることがある
    SendClientMessage(window, XInternAtom(display_, "_NET_ACTIVE_WINDOW", False), 2, static_cast<long>(time), 0, 0);
}

bool X11Desktop::Exists(Window window) {
    if (!display_ || window == 0) return false;
    XWindowAttributes attributes;
    const Status status = XGetWindowAttributes(display_, window, &attributes);
    return SyncAndTakeError(display_) == 0 && status != 0;
}

bool X11Desktop::KeysHeld() {
    if (!display_) return false;
    char keys[32] = {};
    XQueryKeymap(display_, keys);
    const KeySym modifiers[] = {XK_Control_L, XK_Control_R, XK_Alt_L,   XK_Alt_R,   XK_Shift_L,         XK_Shift_R,
                                XK_Super_L,   XK_Super_R,   XK_Meta_L,  XK_Meta_R,  XK_ISO_Level3_Shift, XK_Return, XK_KP_Enter};
    for (KeySym keysym : modifiers) {
        const KeyCode keycode = XKeysymToKeycode(display_, keysym);
        if (keycode != 0 && (keys[keycode / 8] & (1 << (keycode % 8))) != 0) return true;
    }
    return false;
}

bool X11Desktop::SendPaste() {
    if (!display_ || !xtest_) return false;
    const KeyCode control = XKeysymToKeycode(display_, XK_Control_L);
    const KeyCode v = XKeysymToKeycode(display_, XK_v);
    if (control == 0 || v == 0) return false;
    XTestFakeKeyEvent(display_, control, True, CurrentTime);
    XTestFakeKeyEvent(display_, v, True, CurrentTime);
    XTestFakeKeyEvent(display_, v, False, CurrentTime);
    XTestFakeKeyEvent(display_, control, False, CurrentTime);
    return SyncAndTakeError(display_) == 0;
}

std::optional<std::pair<int, int>> X11Desktop::PointerPosition() {
    if (!display_) return std::nullopt;
    Window rootReturn = 0;
    Window child = 0;
    int rootX = 0;
    int rootY = 0;
    int windowX = 0;
    int windowY = 0;
    unsigned mask = 0;
    if (!XQueryPointer(display_, root_, &rootReturn, &child, &rootX, &rootY, &windowX, &windowY, &mask)) {
        return std::nullopt;
    }
    return std::make_pair(rootX, rootY);
}

void X11Desktop::PlaceWindow(Window window, int x, int y) {
    if (!display_ || window == 0) return;
    // 位置を指定したことをウィンドウマネージャに伝える（無いと、表示するときにウィンドウマネージャが置き場所を決め直す）
    XSizeHints* hints = XAllocSizeHints();
    if (hints) {
        long supplied = 0;
        XGetWMNormalHints(display_, window, hints, &supplied);
        hints->flags |= USPosition;
        hints->x = x;
        hints->y = y;
        XSetWMNormalHints(display_, window, hints);
        XFree(hints);
    }
    XMoveWindow(display_, window, x, y);
    SyncAndTakeError(display_);
}

void X11Desktop::KeepAbove(Window window) {
    if (!display_ || window == 0) return;
    // _NET_WM_STATE に「最前面」と「タスクバーに出さない」を足す（1 = 足す、最後の 1 = アプリからの依頼）
    SendClientMessage(window, XInternAtom(display_, "_NET_WM_STATE", False), 1,
                      static_cast<long>(XInternAtom(display_, "_NET_WM_STATE_ABOVE", False)),
                      static_cast<long>(XInternAtom(display_, "_NET_WM_STATE_SKIP_TASKBAR", False)), 1);
}

void X11Desktop::MakeOverlay(Window window) {
    if (!display_ || window == 0) return;
    XSetWindowAttributes attributes = {};
    attributes.override_redirect = True;
    XChangeWindowAttributes(display_, window, CWOverrideRedirect, &attributes);
    // 念のため、キーボードのフォーカスを受けない印も付ける
    if (XWMHints* hints = XAllocWMHints()) {
        hints->flags = InputHint;
        hints->input = False;
        XSetWMHints(display_, window, hints);
        XFree(hints);
    }
    SyncAndTakeError(display_);
}

std::optional<X11Desktop::Rect> X11Desktop::WindowRect(Window window) {
    if (!display_ || window == 0) return std::nullopt;
    Window rootReturn = 0;
    int x = 0;
    int y = 0;
    unsigned width = 0;
    unsigned height = 0;
    unsigned border = 0;
    unsigned depth = 0;
    const Status status = XGetGeometry(display_, window, &rootReturn, &x, &y, &width, &height, &border, &depth);
    Window child = 0;
    int rootX = 0;
    int rootY = 0;
    const bool translated = status && XTranslateCoordinates(display_, window, root_, 0, 0, &rootX, &rootY, &child);
    if (SyncAndTakeError(display_) != 0 || !translated) return std::nullopt;
    return Rect{rootX, rootY, static_cast<int>(width), static_cast<int>(height)};
}

} // namespace iroha::selection
