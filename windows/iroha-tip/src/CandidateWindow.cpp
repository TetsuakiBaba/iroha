#include "CandidateWindow.h"

#include "iroha/unicode.h"

namespace {

constexpr wchar_t kClassName[] = L"IrohaCandidateWindow";

void RegisterWindowClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefWindowProcW; // 実体はサブクラス相当のThunkで差し替える
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    registered = RegisterClassExW(&wc) != 0 ||
                 GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

LRESULT CALLBACK CandidateWindow::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                               LPARAM lParam) {
    auto* self =
        reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (self) {
                RECT client;
                GetClientRect(hwnd, &client);
                self->Paint(dc, client);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE; // クリックされてもフォーカスを奪わない
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

CandidateWindow::~CandidateWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (font_) DeleteObject(font_);
}

void CandidateWindow::EnsureWindow(HWND owner) {
    if (hwnd_ && owner && GetWindow(hwnd_, GW_OWNER) != owner) {
        // ownerは後から変えられないため作り直す
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (hwnd_) return;
    RegisterWindowClass();
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kClassName, L"",
        WS_POPUP | WS_BORDER, 0, 0, 10, 10, owner, nullptr, g_hInst, nullptr);
    if (!hwnd_) {
        IrohaLog(L"CandidateWindow: CreateWindowEx failed: %u", GetLastError());
        return;
    }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(hwnd_, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(&CandidateWindow::WndProcThunk));
}

void CandidateWindow::UpdateFont() {
    const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
    if (font_ && dpi == dpi_) return;
    if (font_) DeleteObject(font_);
    dpi_ = dpi;
    const int height = -MulDiv(10, static_cast<int>(dpi_), 72); // 10pt
    font_ = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Meiryo UI");
    rowHeight_ = MulDiv(20, static_cast<int>(dpi_), 96);
    padding_ = MulDiv(4, static_cast<int>(dpi_), 96);
}

SIZE CandidateWindow::MeasureContent() const {
    SIZE content = {0, 0};
    HDC dc = GetDC(hwnd_);
    HGDIOBJ oldFont = SelectObject(dc, font_);
    for (const auto& item : items_) {
        SIZE size = {};
        GetTextExtentPoint32W(dc, item.c_str(), static_cast<int>(item.size()), &size);
        if (size.cx > content.cx) content.cx = size.cx;
    }
    SelectObject(dc, oldFont);
    ReleaseDC(hwnd_, dc);
    content.cx += padding_ * 4;
    content.cy = static_cast<LONG>(items_.size()) * rowHeight_ + padding_ * 2;
    return content;
}

void CandidateWindow::Paint(HDC dc, const RECT& client) const {
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
    HGDIOBJ oldFont = SelectObject(dc, font_);
    SetBkMode(dc, TRANSPARENT);
    for (size_t i = 0; i < items_.size(); ++i) {
        RECT row = {padding_, padding_ + static_cast<LONG>(i) * rowHeight_,
                    client.right - padding_,
                    padding_ + static_cast<LONG>(i + 1) * rowHeight_};
        if (i == selected_) {
            RECT highlight = {0, row.top, client.right, row.bottom};
            FillRect(dc, &highlight, GetSysColorBrush(COLOR_HIGHLIGHT));
            SetTextColor(dc, GetSysColor(COLOR_HIGHLIGHTTEXT));
        } else {
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        }
        DrawTextW(dc, items_[i].c_str(), static_cast<int>(items_[i].size()), &row,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    }
    SelectObject(dc, oldFont);
}

void CandidateWindow::Show(HWND owner, const RECT& anchor,
                           const std::vector<std::u32string>& candidates,
                           size_t selectedIndex) {
    EnsureWindow(owner);
    if (!hwnd_) return;
    UpdateFont();

    items_.clear();
    for (size_t i = 0; i < candidates.size(); ++i) {
        items_.push_back(std::to_wstring(i + 1) + L"  " +
                         iroha::Utf32ToUtf16(candidates[i]));
    }
    selected_ = selectedIndex;

    const SIZE content = MeasureContent();

    // コンポジションの直下に出す。モニタ作業領域からはみ出すなら直上・右端寄せ
    MONITORINFO monitor = {sizeof(monitor)};
    RECT anchorCopy = anchor;
    GetMonitorInfoW(MonitorFromRect(&anchorCopy, MONITOR_DEFAULTTONEAREST), &monitor);
    int x = anchor.left;
    int y = anchor.bottom + 2;
    if (y + content.cy > monitor.rcWork.bottom) y = anchor.top - content.cy - 2;
    if (x + content.cx > monitor.rcWork.right) x = monitor.rcWork.right - content.cx;
    if (x < monitor.rcWork.left) x = monitor.rcWork.left;
    if (y < monitor.rcWork.top) y = monitor.rcWork.top;

    const bool wasVisible = IsWindowVisible(hwnd_) != FALSE;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, content.cx, content.cy,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, TRUE);
    // ライトディスミス協調のためのIMEイベント
    NotifyWinEvent(wasVisible ? EVENT_OBJECT_IME_CHANGE : EVENT_OBJECT_IME_SHOW, hwnd_,
                   OBJID_CLIENT, CHILDID_SELF);
}

void CandidateWindow::Hide() {
    if (!hwnd_ || !IsWindowVisible(hwnd_)) return;
    ShowWindow(hwnd_, SW_HIDE);
    NotifyWinEvent(EVENT_OBJECT_IME_HIDE, hwnd_, OBJID_CLIENT, CHILDID_SELF);
}
