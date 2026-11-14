#include "ui.h"

#include <algorithm>
#include <objidl.h>
namespace Gdiplus {
using std::max;
using std::min;
} // namespace Gdiplus
#include <commctrl.h>
#include <gdiplus.h>
#include <windowsx.h>

namespace ui {

Theme g_theme;

namespace {

constexpr const wchar_t* kToggleClass = L"IrohaToggle";
constexpr const wchar_t* kHelpClass = L"IrohaHelpButton";
constexpr const wchar_t* kFlyoutClass = L"IrohaFlyout";
constexpr const wchar_t* kPageClass = L"IrohaPage";

HFONT MakeFont(int pixels, int weight, const wchar_t* face) {
    return CreateFontW(-pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

bool FontExists(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    LOGFONTW query = {};
    query.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(query.lfFaceName, face, _TRUNCATE);
    bool found = false;
    EnumFontFamiliesExW(
        dc, &query, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM param) -> int {
            *reinterpret_cast<bool*>(param) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

Gdiplus::Color ToColor(COLORREF color, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

// 角の丸い四角（GDI+。縁は 1 ピクセルの内側に描く）
void FillRoundRect(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border, bool drawBorder) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float x = static_cast<float>(rect.left) + 0.5f;
    const float y = static_cast<float>(rect.top) + 0.5f;
    const float width = static_cast<float>(rect.right - rect.left) - 1.0f;
    const float height = static_cast<float>(rect.bottom - rect.top) - 1.0f;
    const float diameter = std::min(static_cast<float>(radius * 2), std::min(width, height));
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, diameter, diameter, 180, 90);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270, 90);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0, 90);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
    Gdiplus::SolidBrush brush(ToColor(fill));
    graphics.FillPath(&brush, &path);
    if (drawBorder) {
        Gdiplus::Pen pen(ToColor(border), 1.0f);
        graphics.DrawPath(&pen, &path);
    }
}

void FillEllipse(HDC dc, float x, float y, float diameter, COLORREF color) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(ToColor(color));
    graphics.FillEllipse(&brush, x, y, diameter, diameter);
}

int TextWidth(HDC dc, HFONT font, const std::wstring& text) {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size = {};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, old);
    return size.cx;
}

// ---- スイッチ ----

struct ToggleState {
    bool checked = false;
    bool hover = false;
};

LRESULT CALLBACK ToggleProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ToggleState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    const auto click = [hwnd, state] {
        state->checked = !state->checked;
        InvalidateRect(hwnd, nullptr, FALSE);
        SendMessageW(GetParent(hwnd), WM_COMMAND,
                     MAKEWPARAM(static_cast<WORD>(GetDlgCtrlID(hwnd)), BN_CLICKED), reinterpret_cast<LPARAM>(hwnd));
    };
    switch (message) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new ToggleState()));
        break;
    case WM_NCDESTROY:
        delete state;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    case BM_GETCHECK:
        return state && state->checked ? BST_CHECKED : BST_UNCHECKED;
    case BM_SETCHECK:
        if (state) {
            state->checked = wParam == BST_CHECKED;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_GETDLGCODE:
        return DLGC_BUTTON;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) {
            ReleaseCapture();
            POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client = {};
            GetClientRect(hwnd, &client);
            if (PtInRect(&client, point) && IsWindowEnabled(hwnd)) click();
        }
        return 0;
    case WM_KEYUP:
        if (wParam == VK_SPACE && IsWindowEnabled(hwnd)) click();
        return 0;
    case WM_MOUSEMOVE:
        if (state && !state->hover) {
            state->hover = true;
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (state) state->hover = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC target = BeginPaint(hwnd, &paint);
        RECT client = {};
        GetClientRect(hwnd, &client);
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        FillRect(dc, &client, g_theme.cardBrush);
        const Theme& t = g_theme;
        const bool enabled = IsWindowEnabled(hwnd) != FALSE;
        const bool on = state && state->checked;
        // 「オン」「オフ」と、幅 40・高さ 20 の丸いスイッチ（Windows 11 の設定アプリと同じ大きさ）
        const int pillWidth = t.S(40);
        const int pillHeight = t.S(20);
        RECT pill = {client.right - pillWidth - t.S(2), (client.bottom - pillHeight) / 2, client.right - t.S(2),
                     (client.bottom - pillHeight) / 2 + pillHeight};
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(dc, t.body);
        SetTextColor(dc, enabled ? t.text : t.disabledText);
        RECT label = {0, 0, pill.left - t.S(12), client.bottom};
        DrawTextW(dc, on ? L"オン" : L"オフ", -1, &label, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldFont);
        const COLORREF accent = enabled ? t.accent : t.disabledText;
        if (on) {
            FillRoundRect(dc, pill, pillHeight / 2, accent, accent, false);
        } else {
            FillRoundRect(dc, pill, pillHeight / 2, t.card, enabled ? RGB(0x86, 0x86, 0x86) : t.disabledText, true);
        }
        const float knob = static_cast<float>(t.S(state && state->hover && enabled ? 14 : 12));
        const float knobX = on ? pill.right - t.S(4) - knob : pill.left + t.S(4);
        const float knobY = pill.top + (pillHeight - knob) / 2.0f;
        FillEllipse(dc, knobX, knobY, knob, on ? RGB(0xFF, 0xFF, 0xFF) : (enabled ? RGB(0x5D, 0x5D, 0x5D) : t.disabledText));
        if (GetFocus() == hwnd) {
            RECT focus = pill;
            InflateRect(&focus, t.S(3), t.S(3));
            DrawFocusRect(dc, &focus);
        }
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// ---- 「?」ボタンと説明の小窓 ----

HWND g_flyout = nullptr;

LRESULT CALLBACK HelpProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* text = reinterpret_cast<std::wstring*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(new std::wstring(static_cast<const wchar_t*>(create->lpCreateParams))));
        break;
    }
    case WM_NCDESTROY:
        delete text;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    case WM_LBUTTONUP:
        if (text) ShowFlyout(hwnd, *text);
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        SetPropW(hwnd, L"hover", reinterpret_cast<HANDLE>(1));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSELEAVE:
        RemovePropW(hwnd, L"hover");
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client = {};
        GetClientRect(hwnd, &client);
        // 地は親（ページ）の地。見出しの横にもカードの中にも置くので、親に尋ねる
        HBRUSH background = reinterpret_cast<HBRUSH>(
            SendMessageW(GetParent(hwnd), WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(hwnd)));
        FillRect(dc, &client, background ? background : g_theme.cardBrush);
        const bool hover = GetPropW(hwnd, L"hover") != nullptr;
        const COLORREF color = hover ? g_theme.accent : g_theme.secondaryText;
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(ToColor(color), static_cast<float>(g_theme.S(1)));
        const float size = static_cast<float>(std::min(client.right, client.bottom) - g_theme.S(2));
        graphics.DrawEllipse(&pen, (client.right - size) / 2.0f, (client.bottom - size) / 2.0f, size, size);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        HGDIOBJ old = SelectObject(dc, g_theme.badge);
        DrawTextW(dc, L"?", 1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        EndPaint(hwnd, &paint);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK FlyoutProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) DestroyWindow(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_LBUTTONUP:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_flyout == hwnd) g_flyout = nullptr;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client = {};
        GetClientRect(hwnd, &client);
        HBRUSH white = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
        FillRect(dc, &client, white);
        DeleteObject(white);
        HPEN pen = CreatePen(PS_SOLID, 1, g_theme.cardBorder);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 0, 0, client.right, client.bottom);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        wchar_t text[4096];
        GetWindowTextW(hwnd, text, ARRAYSIZE(text));
        RECT area = client;
        InflateRect(&area, -g_theme.S(14), -g_theme.S(12));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_theme.text);
        HGDIOBJ oldFont = SelectObject(dc, g_theme.caption);
        DrawTextW(dc, text, -1, &area, DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        EndPaint(hwnd, &paint);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

void Theme::Rebuild(UINT newDpi) {
    Release();
    dpi = newDpi;
    const wchar_t* face = L"Yu Gothic UI";
    pageTitle = MakeFont(S(28), FW_SEMIBOLD, face);
    header = MakeFont(S(14), FW_SEMIBOLD, face);
    body = MakeFont(S(14), FW_NORMAL, face);
    caption = MakeFont(S(12), FW_NORMAL, face);
    badge = MakeFont(S(11), FW_SEMIBOLD, face);
    icon = MakeFont(S(16), FW_NORMAL, FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets");
    backgroundBrush = CreateSolidBrush(background);
    cardBrush = CreateSolidBrush(card);
}

void Theme::Release() {
    for (HFONT* font : {&pageTitle, &header, &body, &caption, &badge, &icon}) {
        if (*font) DeleteObject(*font);
        *font = nullptr;
    }
    if (backgroundBrush) DeleteObject(backgroundBrush);
    if (cardBrush) DeleteObject(cardBrush);
    backgroundBrush = nullptr;
    cardBrush = nullptr;
}

void RegisterClasses(HINSTANCE instance) {
    WNDCLASSEXW toggle = {sizeof(toggle)};
    toggle.lpfnWndProc = ToggleProc;
    toggle.hInstance = instance;
    toggle.hCursor = LoadCursorW(nullptr, IDC_HAND);
    toggle.lpszClassName = kToggleClass;
    RegisterClassExW(&toggle);
    WNDCLASSEXW help = {sizeof(help)};
    help.lpfnWndProc = HelpProc;
    help.hInstance = instance;
    help.hCursor = LoadCursorW(nullptr, IDC_HAND);
    help.lpszClassName = kHelpClass;
    RegisterClassExW(&help);
    WNDCLASSEXW flyout = {sizeof(flyout)};
    flyout.lpfnWndProc = FlyoutProc;
    flyout.hInstance = instance;
    flyout.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    flyout.lpszClassName = kFlyoutClass;
    flyout.style = CS_DROPSHADOW;
    RegisterClassExW(&flyout);
    WNDCLASSEXW page = {sizeof(page)};
    page.lpfnWndProc = Page::Proc;
    page.hInstance = instance;
    page.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    page.lpszClassName = kPageClass;
    RegisterClassExW(&page);
}

HWND CreateControl(HWND parent, const wchar_t* className, const wchar_t* text, DWORD style, int id, DWORD exStyle) {
    HWND control = CreateWindowExW(exStyle, className, text, WS_CHILD | style, 0, 0, 10, 10, parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_theme.body), TRUE);
    return control;
}

HWND CreateToggle(HWND parent, int id) {
    return CreateWindowExW(0, kToggleClass, L"", WS_CHILD | WS_TABSTOP, 0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

HWND CreateHelpButton(HWND parent, const std::wstring& text) {
    return CreateWindowExW(0, kHelpClass, L"", WS_CHILD, 0, 0, 10, 10, parent, nullptr, GetModuleHandleW(nullptr),
                           const_cast<wchar_t*>(text.c_str()));
}

void ShowFlyout(HWND anchor, const std::wstring& text) {
    if (g_flyout) DestroyWindow(g_flyout);
    const int width = g_theme.S(360);
    HDC dc = GetDC(anchor);
    HGDIOBJ old = SelectObject(dc, g_theme.caption);
    RECT measure = {0, 0, width - g_theme.S(28), 0};
    DrawTextW(dc, text.c_str(), -1, &measure, DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, old);
    ReleaseDC(anchor, dc);
    const int height = measure.bottom + g_theme.S(24);
    RECT anchorRect = {};
    GetWindowRect(anchor, &anchorRect);
    MONITORINFO monitor = {sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(&anchorRect, MONITOR_DEFAULTTONEAREST), &monitor);
    int x = std::min(static_cast<int>(anchorRect.left) - g_theme.S(12), static_cast<int>(monitor.rcWork.right) - width - 8);
    x = std::max(x, static_cast<int>(monitor.rcWork.left) + 8);
    int y = anchorRect.bottom + g_theme.S(6);
    if (y + height > monitor.rcWork.bottom) y = anchorRect.top - height - g_theme.S(6);
    g_flyout = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kFlyoutClass, text.c_str(), WS_POPUP, x, y, width,
                               height, GetAncestor(anchor, GA_ROOT), nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_flyout) return;
    ShowWindow(g_flyout, SW_SHOW);
    SetForegroundWindow(g_flyout);
    SetFocus(g_flyout);
}

// ---- ページ ----

Page::Page(HWND parent, const std::wstring& title) : title_(title) {
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, kPageClass, title.c_str(), WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0,
                            10, 10, parent, nullptr, GetModuleHandleW(nullptr), this);
}

Page::~Page() {
    if (hwnd_) DestroyWindow(hwnd_);
}

Page::Row* Page::AddHeader(const std::wstring& title, const std::wstring& help, bool developer) {
    auto row = std::make_unique<Row>();
    row->kind = Row::Kind::Header;
    row->title = title;
    row->help = help;
    row->developer = developer;
    if (!help.empty()) row->helpButton = CreateHelpButton(hwnd_, help);
    rows_.push_back(std::move(row));
    return rows_.back().get();
}

Page::Row* Page::AddItem(const std::wstring& title, const std::wstring& description, const std::wstring& help,
                         bool developer) {
    auto row = std::make_unique<Row>();
    row->title = title;
    row->description = description;
    row->help = help;
    row->developer = developer;
    if (!help.empty()) row->helpButton = CreateHelpButton(hwnd_, help);
    rows_.push_back(std::move(row));
    return rows_.back().get();
}

Page::Row* Page::AddNote(const std::wstring& text) {
    auto row = std::make_unique<Row>();
    row->kind = Row::Kind::Note;
    row->description = text;
    rows_.push_back(std::move(row));
    return rows_.back().get();
}

Page::Row* Page::AddFill(HWND control, int minHeight) {
    auto row = std::make_unique<Row>();
    row->kind = Row::Kind::Fill;
    row->below.push_back({Control{control, -1, minHeight}});
    row->minHeight = minHeight;
    rows_.push_back(std::move(row));
    return rows_.back().get();
}

void Page::SetDescription(Row* row, const std::wstring& text, bool warning) {
    if (row->description == text && row->warning == warning) return;
    row->description = text;
    row->warning = warning;
    Relayout();
}

void Page::Show(bool visible) {
    ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
    if (visible) Relayout();
}

// スクロールは並べ直さず、ページの絵と部品を画素ごと動かし（ScrollWindowEx の SW_SCROLLCHILDREN）、
// 新しく見えた所だけをその場で描く。並べ直して全部を描き直す予定に積む形では、ホイールやつまみのドラッグで
// スクロールが続くと描き直しが追いつかず、動いている間は部品が残像のように見えた（2026-11-14）
void Page::ScrollTo(int position) {
    RECT client = {};
    GetClientRect(hwnd_, &client);
    const int next = std::max(0, std::min(position, contentHeight_ - static_cast<int>(client.bottom)));
    const int delta = scroll_ - next;
    if (delta == 0) return;
    scroll_ = next;
    SetScrollPos(hwnd_, SB_VERT, scroll_, TRUE);
    HRGN exposed = CreateRectRgn(0, 0, 0, 0);
    ScrollWindowEx(hwnd_, 0, delta, nullptr, nullptr, exposed, nullptr, SW_SCROLLCHILDREN);
    RedrawWindow(hwnd_, nullptr, exposed, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    DeleteObject(exposed);
}

int Page::MeasureText(HFONT font, const std::wstring& text, int width) const {
    if (text.empty() || width <= 0) return 0;
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font);
    RECT rect = {0, 0, width, 0};
    DrawTextW(dc, text.c_str(), -1, &rect, DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    return rect.bottom;
}

// 項目を並べる。部品は内容の座標からスクロールの分を引いた位置に置く
void Page::Relayout() {
    const Theme& t = g_theme;
    RECT client = {};
    GetClientRect(hwnd_, &client);
    const int margin = t.S(36);
    const int width = std::min<int>(client.right - margin * 2, t.S(1000));
    const int left = margin;
    const int pad = t.S(16);
    const int gap = t.S(8);

    const auto setControl = [&](const Control& control, int x, int y, int w, int h, bool show, bool enable) {
        if (!control.hwnd) return;
        MoveWindow(control.hwnd, x, y - scroll_, w, h + t.S(control.dropHeight), FALSE);
        EnableWindow(control.hwnd, enable);
        ShowWindow(control.hwnd, show ? SW_SHOWNA : SW_HIDE);
    };

    // 2 回並べる: 1 回目で全体の高さを求め、余りを Fill の行に回す
    int extra = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const bool place = pass == 1;
        int y = t.S(24) + t.S(40) + t.S(12); // ページの題の下
        bool firstSection = true;
        for (const auto& owned : rows_) {
            Row& row = *owned;
            row.shown = !row.visible || row.visible();
            const bool enabled = !row.enabled || row.enabled();
            if (!row.shown) {
                if (place) {
                    if (row.helpButton) ShowWindow(row.helpButton, SW_HIDE);
                    for (const Control& c : row.right) setControl(c, 0, 0, 0, 0, false, false);
                    for (const auto& line : row.below) {
                        for (const Control& c : line) setControl(c, 0, 0, 0, 0, false, false);
                    }
                }
                continue;
            }
            switch (row.kind) {
            case Row::Kind::Header: {
                if (!firstSection) y += t.S(20);
                firstSection = false;
                const int height = t.S(28);
                row.rect = {left, y, left + width, y + height};
                if (place && row.helpButton) {
                    HDC dc = GetDC(hwnd_);
                    const int titleWidth = TextWidth(dc, t.header, row.title);
                    ReleaseDC(hwnd_, dc);
                    const int size = t.S(18);
                    MoveWindow(row.helpButton, left + t.S(2) + titleWidth + t.S(6), y + (height - size) / 2 - scroll_,
                               size, size, FALSE);
                    ShowWindow(row.helpButton, SW_SHOWNA);
                }
                y += height + t.S(4);
                break;
            }
            case Row::Kind::Note: {
                const int height = MeasureText(t.caption, row.description, width);
                row.rect = {left, y, left + width, y + height};
                y += height + t.S(10);
                break;
            }
            case Row::Kind::Item:
            case Row::Kind::Fill: {
                int rightWidth = 0;
                int rightHeight = 0;
                for (const Control& c : row.right) {
                    rightWidth += t.S(c.width) + (rightWidth > 0 ? gap : 0);
                    rightHeight = std::max(rightHeight, t.S(c.height));
                }
                const int textWidth = width - pad * 2 - (rightWidth > 0 ? rightWidth + t.S(24) : 0);
                const int titleHeight = row.title.empty() ? 0 : t.S(20);
                const int descriptionHeight = MeasureText(t.caption, row.description, textWidth);
                const int textHeight = titleHeight + (descriptionHeight > 0 ? (titleHeight > 0 ? t.S(2) : 0) + descriptionHeight : 0);
                const int top = std::max(textHeight, rightHeight);
                int belowHeight = 0;
                for (const auto& line : row.below) {
                    int lineHeight = 0;
                    for (const Control& c : line) {
                        int h = t.S(c.height);
                        if (row.kind == Row::Kind::Fill) h += extra;
                        lineHeight = std::max(lineHeight, h);
                    }
                    belowHeight += lineHeight + (belowHeight > 0 ? gap : 0);
                }
                const int vertical = t.S(top > 0 ? 14 : 12);
                int height = vertical * 2 + top + (belowHeight > 0 ? (top > 0 ? t.S(12) : 0) + belowHeight : 0);
                if (top > 0) height = std::max(height, t.S(64));
                row.rect = {left, y, left + width, y + height};
                if (place) {
                    const int contentTop = y + vertical;
                    // 右側の部品（上の段の中ほど）
                    int x = left + width - pad - rightWidth;
                    for (const Control& c : row.right) {
                        const int h = t.S(c.height);
                        setControl(c, x, contentTop + (top - h) / 2, t.S(c.width), h, true, enabled);
                        x += t.S(c.width) + gap;
                    }
                    // 「?」は題の右
                    if (row.helpButton) {
                        HDC dc = GetDC(hwnd_);
                        const int titleWidth = TextWidth(dc, t.body, row.title);
                        ReleaseDC(hwnd_, dc);
                        const int size = t.S(18);
                        const int titleTop = contentTop + (top - textHeight) / 2;
                        MoveWindow(row.helpButton, left + pad + titleWidth + t.S(6),
                                   titleTop + (titleHeight - size) / 2 - scroll_, size, size, FALSE);
                        ShowWindow(row.helpButton, SW_SHOWNA);
                    }
                    // 下の段
                    int lineY = contentTop + (top > 0 ? top + t.S(12) : 0);
                    for (const auto& line : row.below) {
                        int lineX = left + pad;
                        int lineHeight = 0;
                        for (const Control& c : line) {
                            int h = t.S(c.height);
                            if (row.kind == Row::Kind::Fill) h += extra;
                            const int w = c.width < 0 ? left + width - pad - lineX : t.S(c.width);
                            setControl(c, lineX, lineY, w, h, true, enabled);
                            lineX += w + gap;
                            lineHeight = std::max(lineHeight, h);
                        }
                        lineY += lineHeight + gap;
                    }
                }
                y += height + t.S(4);
                break;
            }
            }
        }
        const int total = y + t.S(24);
        if (pass == 0) {
            const bool hasFill = std::any_of(rows_.begin(), rows_.end(), [](const auto& r) {
                return r->kind == Row::Kind::Fill && (!r->visible || r->visible());
            });
            extra = hasFill ? std::max(0, static_cast<int>(client.bottom) - total) : 0;
        } else {
            contentHeight_ = total;
        }
    }
    // スクロールの幅
    scroll_ = std::max(0, std::min(scroll_, contentHeight_ - static_cast<int>(client.bottom)));
    SCROLLINFO info = {sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMin = 0;
    info.nMax = contentHeight_ - 1;
    info.nPage = static_cast<UINT>(client.bottom);
    info.nPos = scroll_;
    SetScrollInfo(hwnd_, SB_VERT, &info, TRUE);
    // 部品は再描画なしで動かしている（MoveWindow の FALSE）。ページは子の上を描かない（WS_CLIPCHILDREN）ので、
    // ページだけを描き直すと、動いた部品の所に前の絵が残り、ホバーで部品が自分を描き直すまで見えなかった
    // （2026-11-14）。子もまとめて描き直す
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void Page::Paint(HDC dc) {
    const Theme& t = g_theme;
    RECT client = {};
    GetClientRect(hwnd_, &client);
    FillRect(dc, &client, t.backgroundBrush);
    SetBkMode(dc, TRANSPARENT);
    const int margin = t.S(36);
    // ページの題
    RECT titleRect = {margin, t.S(24) - scroll_, client.right - margin, t.S(24) + t.S(40) - scroll_};
    HGDIOBJ oldFont = SelectObject(dc, t.pageTitle);
    SetTextColor(dc, t.text);
    DrawTextW(dc, title_.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    const auto drawBadge = [&](int x, int centerY) {
        const std::wstring text = L"開発者";
        const int textWidth = TextWidth(dc, t.badge, text);
        RECT pill = {x, centerY - t.S(9), x + textWidth + t.S(12), centerY + t.S(9)};
        FillRoundRect(dc, pill, t.S(9), t.developerFill, t.developerFill, false);
        SelectObject(dc, t.badge);
        SetTextColor(dc, t.developer);
        DrawTextW(dc, text.c_str(), -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    };
    for (const auto& owned : rows_) {
        const Row& row = *owned;
        if (!row.shown) continue;
        RECT rect = row.rect;
        OffsetRect(&rect, 0, -scroll_);
        if (rect.bottom < 0 || rect.top > client.bottom) continue;
        const bool enabled = !row.enabled || row.enabled();
        switch (row.kind) {
        case Row::Kind::Header: {
            SelectObject(dc, t.header);
            SetTextColor(dc, t.text);
            RECT text = rect;
            text.left += t.S(2);
            DrawTextW(dc, row.title.c_str(), -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (row.developer) {
                int x = text.left + TextWidth(dc, t.header, row.title) + t.S(8);
                if (row.helpButton) x += t.S(24);
                drawBadge(x, (rect.top + rect.bottom) / 2);
            }
            break;
        }
        case Row::Kind::Note: {
            SelectObject(dc, t.caption);
            SetTextColor(dc, row.warning ? t.warning : t.secondaryText);
            DrawTextW(dc, row.description.c_str(), -1, &rect, DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
            break;
        }
        case Row::Kind::Item:
        case Row::Kind::Fill: {
            FillRoundRect(dc, rect, t.S(6), t.card, t.cardBorder, true);
            const int pad = t.S(16);
            int rightWidth = 0;
            int rightHeight = 0;
            for (const Control& c : row.right) {
                rightWidth += t.S(c.width) + (rightWidth > 0 ? t.S(8) : 0);
                rightHeight = std::max(rightHeight, t.S(c.height));
            }
            const int textWidth = rect.right - rect.left - pad * 2 - (rightWidth > 0 ? rightWidth + t.S(24) : 0);
            const int titleHeight = row.title.empty() ? 0 : t.S(20);
            const int descriptionHeight = MeasureText(t.caption, row.description, textWidth);
            const int textHeight =
                titleHeight + (descriptionHeight > 0 ? (titleHeight > 0 ? t.S(2) : 0) + descriptionHeight : 0);
            const int top = std::max(textHeight, rightHeight);
            const int vertical = t.S(top > 0 ? 14 : 12);
            int textTop = rect.top + vertical + (top - textHeight) / 2;
            if (!row.title.empty()) {
                SelectObject(dc, t.body);
                SetTextColor(dc, enabled ? t.text : t.disabledText);
                RECT title = {rect.left + pad, textTop, rect.left + pad + textWidth, textTop + titleHeight};
                DrawTextW(dc, row.title.c_str(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                if (row.developer) {
                    int x = title.left + TextWidth(dc, t.body, row.title) + t.S(8);
                    if (row.helpButton) x += t.S(24);
                    drawBadge(x, textTop + titleHeight / 2);
                }
                textTop += titleHeight + t.S(2);
            }
            if (descriptionHeight > 0) {
                SelectObject(dc, t.caption);
                SetTextColor(dc, row.warning ? t.warning : enabled ? t.secondaryText : t.disabledText);
                RECT description = {rect.left + pad, textTop, rect.left + pad + textWidth, textTop + descriptionHeight};
                DrawTextW(dc, row.description.c_str(), -1, &description, DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
            }
            break;
        }
        }
    }
    SelectObject(dc, oldFont);
}

LRESULT CALLBACK Page::Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    }
    auto* page = reinterpret_cast<Page*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!page || !page->hwnd_) {
        if (page && message != WM_NCCREATE) page->hwnd_ = hwnd;
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return page->Handle(message, wParam, lParam);
}

LRESULT Page::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        Relayout();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC target = BeginPaint(hwnd_, &paint);
        RECT client = {};
        GetClientRect(hwnd_, &client);
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
        HGDIOBJ old = SelectObject(dc, bitmap);
        Paint(dc);
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(hwnd_, &paint);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO info = {sizeof(info), SIF_ALL};
        GetScrollInfo(hwnd_, SB_VERT, &info);
        int position = scroll_;
        switch (LOWORD(wParam)) {
        case SB_LINEUP: position -= g_theme.S(40); break;
        case SB_LINEDOWN: position += g_theme.S(40); break;
        case SB_PAGEUP: position -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: position = info.nTrackPos; break;
        case SB_TOP: position = 0; break;
        case SB_BOTTOM: position = contentHeight_; break;
        default: break;
        }
        ScrollTo(position);
        return 0;
    }
    case WM_MOUSEWHEEL:
        ScrollTo(scroll_ - GET_WHEEL_DELTA_WPARAM(wParam) * g_theme.S(48) / WHEEL_DELTA);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        // カードの中の部品の地はカードの色、見出しの横の「?」はページの地
        HWND control = reinterpret_cast<HWND>(lParam);
        HDC dc = reinterpret_cast<HDC>(wParam);
        bool onCard = true;
        for (const auto& row : rows_) {
            if (row->helpButton == control && row->kind == Row::Kind::Header) onCard = false;
        }
        SetBkColor(dc, onCard ? g_theme.card : g_theme.background);
        SetTextColor(dc, g_theme.text);
        return reinterpret_cast<LRESULT>(onCard ? g_theme.cardBrush : g_theme.backgroundBrush);
    }
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_HSCROLL:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
        // 部品の知らせは主の窓で処理する
        return SendMessageW(GetParent(hwnd_), message, wParam, lParam);
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

} // namespace ui
