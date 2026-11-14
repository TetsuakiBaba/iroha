#include "key_check.h"

#include <cstdio>
#include <string>

#include "key_shortcut.h"
#include <commctrl.h>

namespace key_check {

namespace {

constexpr wchar_t kWindowClass[] = L"IrohaKeyCheck";
constexpr UINT WM_APP_KEY = WM_APP + 1; // wParam: 仮想キーコード、lParam: スキャンコード | 拡張 << 8 | 離した << 9 | 修飾キー << 16
constexpr int kMaxLines = 200;

HWND g_window = nullptr;
HWND g_list = nullptr;
HHOOK g_hook = nullptr;
iroha::Config g_config;

std::wstring KeyName(UINT vk) {
    switch (vk) {
    case VK_SPACE: return L"Space";
    case VK_RETURN: return L"Enter";
    case VK_ESCAPE: return L"Esc";
    case VK_BACK: return L"Backspace";
    case VK_TAB: return L"Tab";
    case VK_SHIFT: case VK_LSHIFT: return L"Shift（左）";
    case VK_RSHIFT: return L"Shift（右）";
    case VK_CONTROL: case VK_LCONTROL: return L"Ctrl（左）";
    case VK_RCONTROL: return L"Ctrl（右）";
    case VK_MENU: case VK_LMENU: return L"Alt（左）";
    case VK_RMENU: return L"Alt（右）";
    case VK_LWIN: return L"Windows（左）";
    case VK_RWIN: return L"Windows（右）";
    case VK_CAPITAL: return L"CapsLock";
    case 0x15: return L"VK_KANA（かな）";
    case 0x16: return L"VK_IME_ON（IME オン）";
    case 0x19: return L"VK_KANJI（半角/全角）";
    case 0x1A: return L"VK_IME_OFF（IME オフ）";
    case 0x1C: return L"VK_CONVERT（変換）";
    case 0x1D: return L"VK_NONCONVERT（無変換）";
    case 0xF0: return L"VK_DBE_ALPHANUMERIC（英数）";
    case 0xF1: return L"VK_DBE_KATAKANA（カタカナ）";
    case 0xF2: return L"VK_DBE_HIRAGANA（ひらがな）";
    case 0xF3: return L"VK_DBE_SBCSCHAR（半角/全角）";
    case 0xF4: return L"VK_DBE_DBCSCHAR（半角/全角）";
    case VK_OEM_3: return L"VK_OEM_3（` / @）";
    case VK_OEM_PERIOD: return L".";
    case VK_OEM_COMMA: return L",";
    case VK_LEFT: return L"←";
    case VK_RIGHT: return L"→";
    case VK_UP: return L"↑";
    case VK_DOWN: return L"↓";
    default: break;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    return L"";
}

bool IsModifier(UINT vk) {
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN:
        return true;
    default:
        return false;
    }
}

bool Matches(const std::string& text, UINT vk, bool control, bool alt, bool shift) {
    const auto shortcut = iroha::KeyShortcut::Parse(text);
    return shortcut && shortcut->vk == vk && shortcut->control == control && shortcut->alt == alt && shortcut->shift == shift;
}

// iroha の TIP がこのキーをどう扱うか（TextService::DecideKey のうち、入力していないときに効くもの）
std::wstring Meaning(UINT vk, UINT scan, bool control, bool alt, bool shift) {
    if (scan == 0x72 || vk == 0x16) return L"かなにする（Mac の「かな」キー・IME オン）";
    if (scan == 0x71 || vk == 0x1A) return L"英数にする（Mac の「英数」キー・IME オフ）";
    if (Matches(g_config.inputModeHotkey, vk, control, alt, shift)) return L"かな・英数の切り替え（設定のキー）";
    switch (vk) {
    case 0x19: case 0xF3: case 0xF4: return L"かな・英数の切り替え（半角/全角）";
    case 0xF1: case 0xF2: return L"かなにする";
    case 0xF0: return L"英数にする";
    case VK_OEM_3: if (alt) return L"かな・英数の切り替え（Alt+`）"; break;
    default: break;
    }
    if (Matches(g_config.privateModeHotkey, vk, control, alt, shift)) return L"プライベートモードの切り替え";
    if (control && !alt && vk == VK_OEM_PERIOD) return L"句読点の切り替え";
    return L"";
}

LRESULT CALLBACK HookProc(int code, WPARAM wParam, LPARAM lParam) {
    // 窓が前面にあるときだけ見る（ほかのアプリで打ったキーは見ない）。ここでは何も待たない
    if (code == HC_ACTION && g_window && GetForegroundWindow() == g_window) {
        const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
        const DWORD modifiers = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 1u : 0u) |
                                ((GetAsyncKeyState(VK_MENU) & 0x8000) ? 2u : 0u) |
                                ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 4u : 0u) |
                                (((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) ? 8u : 0u);
        const LPARAM info = static_cast<LPARAM>((key->scanCode & 0xFF) | ((key->flags & LLKHF_EXTENDED) ? 0x100 : 0) |
                                                (up ? 0x200 : 0) | (modifiers << 16));
        PostMessageW(g_window, WM_APP_KEY, key->vkCode, info);
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void AddLine(const std::wstring& line) {
    if (!g_list) return;
    if (SendMessageW(g_list, LB_GETCOUNT, 0, 0) >= kMaxLines) SendMessageW(g_list, LB_DELETESTRING, 0, 0);
    const LRESULT index = SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(g_list, LB_SETTOPINDEX, static_cast<WPARAM>(index), 0);
}

void OnKey(UINT vk, LPARAM info) {
    const UINT scan = static_cast<UINT>(info & 0xFF);
    const bool extended = (info & 0x100) != 0;
    const bool up = (info & 0x200) != 0;
    const DWORD modifiers = static_cast<DWORD>(info >> 16);
    const bool control = (modifiers & 1) != 0, alt = (modifiers & 2) != 0, shift = (modifiers & 4) != 0, win = (modifiers & 8) != 0;
    if (up && !IsModifier(vk)) return; // 離したのは修飾キーだけ出す（押したまま別のキーを押したかを見るため）
    std::wstring keys;
    if (!IsModifier(vk)) {
        if (control) keys += L"Ctrl+";
        if (alt) keys += L"Alt+";
        if (shift) keys += L"Shift+";
        if (win) keys += L"Windows+";
    }
    const std::wstring name = KeyName(vk);
    keys += name.empty() ? L"（名前なし）" : name;
    wchar_t codes[96];
    std::swprintf(codes, 96, L"  仮想キー 0x%02X・スキャンコード 0x%02X%s%s", vk, scan, extended ? L"（拡張）" : L"",
                  up ? L"・離した" : L"");
    std::wstring line = keys + codes;
    if (!up) {
        const std::wstring meaning = Meaning(vk, scan, control, alt, shift);
        if (!meaning.empty()) line += L"  → iroha: " + meaning;
    }
    AddLine(line);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        const UINT dpi = GetDpiForWindow(hwnd);
        HWND note = CreateWindowExW(0, L"STATIC",
                                    L"この窓を前面にしたままキーを押すと、Windows に届いたキーを下に出します（この窓が前面の"
                                    L"ときだけ見ます）。届いていないキーは、リモートデスクトップのアプリや Mac の側が使っています。",
                                    WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT |
                                                                         LBS_NOSEL,
                                 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        HFONT font = CreateFontW(-MulDiv(10, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Yu Gothic UI");
        SendMessageW(note, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        SendMessageW(g_list, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        SetPropW(hwnd, L"IrohaKeyCheckFont", font);
        SetPropW(hwnd, L"IrohaKeyCheckNote", note);
        g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, HookProc, GetModuleHandleW(nullptr), 0);
        if (!g_hook) AddLine(L"キーを見る仕組みを始められませんでした");
        return 0;
    }
    case WM_SIZE: {
        const UINT dpi = GetDpiForWindow(hwnd);
        const int margin = MulDiv(12, static_cast<int>(dpi), 96);
        const int noteHeight = MulDiv(44, static_cast<int>(dpi), 96);
        const int width = LOWORD(lParam);
        const int height = HIWORD(lParam);
        MoveWindow(static_cast<HWND>(GetPropW(hwnd, L"IrohaKeyCheckNote")), margin, margin, width - margin * 2, noteHeight,
                   TRUE);
        MoveWindow(g_list, margin, margin * 2 + noteHeight, width - margin * 2, height - margin * 3 - noteHeight, TRUE);
        return 0;
    }
    case WM_APP_KEY:
        OnKey(static_cast<UINT>(wParam), lParam);
        return 0;
    case WM_DESTROY:
        if (g_hook) UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        if (HFONT font = static_cast<HFONT>(RemovePropW(hwnd, L"IrohaKeyCheckFont"))) DeleteObject(font);
        RemovePropW(hwnd, L"IrohaKeyCheckNote");
        g_window = nullptr;
        g_list = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

void Show(HWND owner, const iroha::Config& config) {
    g_config = config;
    if (g_window) {
        SetForegroundWindow(g_window);
        return;
    }
    static bool registered = false;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kWindowClass;
        LoadIconMetric(instance, MAKEINTRESOURCEW(1), LIM_SMALL, &wc.hIconSm);
        registered = RegisterClassExW(&wc) != 0;
    }
    const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    g_window = CreateWindowExW(0, kWindowClass, L"キーの確認 - iroha", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               MulDiv(760, static_cast<int>(dpi), 96), MulDiv(480, static_cast<int>(dpi), 96), owner,
                               nullptr, instance, nullptr);
    if (!g_window) return;
    ShowWindow(g_window, SW_SHOW);
    SetForegroundWindow(g_window);
}

} // namespace key_check
