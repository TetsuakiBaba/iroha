#include "CandidateWindow.h"

#include <algorithm>
#include <cmath>

#include "iroha/unicode.h"

namespace {

constexpr wchar_t kClassName[] = L"IrohaCandidateWindow";
constexpr wchar_t kTextFamily[] = L"Meiryo UI";
constexpr wchar_t kEmojiFamily[] = L"Segoe UI Emoji";

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

template <typename T>
void SafeRelease(T** pointer) {
    if (*pointer) {
        (*pointer)->Release();
        *pointer = nullptr;
    }
}

D2D1_COLOR_F SystemColor(int index) {
    const COLORREF color = GetSysColor(index);
    return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                        GetBValue(color) / 255.0f);
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
    ReleaseLayouts();
    SafeRelease(&textFormat_);
    SafeRelease(&footerFormat_);
    SafeRelease(&annotationFormat_);
    SafeRelease(&renderTarget_);
    SafeRelease(&dwriteFactory_);
    SafeRelease(&d2dFactory_);
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
                        CLEARTYPE_QUALITY, DEFAULT_PITCH, kTextFamily);
    rowHeight_ = MulDiv(20, static_cast<int>(dpi_), 96);
    padding_ = MulDiv(4, static_cast<int>(dpi_), 96);
    // DirectWrite の文字の大きさは GDI の文字の高さ（em）と同じにする
    fontSize_ = 10.0f * static_cast<float>(dpi_) / 72.0f;
    SafeRelease(&textFormat_);
    SafeRelease(&footerFormat_);
    SafeRelease(&annotationFormat_);
}

// Direct2D・DirectWrite を用意する（文字の大きさが変わったら書式を作り直す）
bool CandidateWindow::EnsureDirectWrite() {
    if (!directWriteTried_) {
        directWriteTried_ = true;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_)) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&dwriteFactory_)))) {
            IrohaLog(L"CandidateWindow: Direct2D/DirectWrite unavailable (GDI fallback)");
            SafeRelease(&d2dFactory_);
            SafeRelease(&dwriteFactory_);
        }
    }
    if (!d2dFactory_ || !dwriteFactory_) return false;
    if (!textFormat_) {
        if (FAILED(dwriteFactory_->CreateTextFormat(
                kTextFamily, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, fontSize_, L"ja-jp", &textFormat_))) {
            return false;
        }
        textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (!footerFormat_) {
        if (FAILED(dwriteFactory_->CreateTextFormat(
                kTextFamily, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, fontSize_, L"ja-jp", &footerFormat_))) {
            return false;
        }
        footerFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        footerFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        footerFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    if (!annotationFormat_) {
        // 印は候補より小さく出す
        if (FAILED(dwriteFactory_->CreateTextFormat(
                kTextFamily, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, fontSize_ * 0.8f, L"ja-jp", &annotationFormat_))) {
            return false;
        }
        annotationFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        annotationFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (!renderTarget_) {
        // 画面の DC にそのまま描く。座標はピクセルのまま使う（DPI は 96 に固定し、文字の大きさで合わせる）
        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        if (FAILED(d2dFactory_->CreateDCRenderTarget(&properties, &renderTarget_))) return false;
    }
    return true;
}

bool CandidateWindow::IsEmojiText(const std::wstring& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        // 絵文字の異体字セレクタ・ZWJ・囲みのキーキャップ・U+1F000 以上（サロゲートの上位が D83C〜D83E）
        if (c == 0xFE0F || c == 0x200D || c == 0x20E3) return true;
        if (c >= 0xD83C && c <= 0xD83E) return true;
    }
    return false;
}

void CandidateWindow::ReleaseLayouts() {
    for (Row& row : rows_) {
        SafeRelease(&row.layout);
        SafeRelease(&row.annotationLayout);
    }
    SafeRelease(&footerLayout_);
}

// 行ごとの文字の配置を作る（番号は Meiryo UI、絵文字の候補は Segoe UI Emoji）
void CandidateWindow::BuildLayouts() {
    ReleaseLayouts();
    if (!EnsureDirectWrite()) return;
    const float height = static_cast<float>(rowHeight_);
    for (Row& row : rows_) {
        const std::wstring text = row.label + row.text;
        if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                    textFormat_, 10000.0f, height, &row.layout))) {
            row.layout = nullptr;
            continue;
        }
        if (IsEmojiText(row.text)) {
            row.layout->SetFontFamilyName(
                kEmojiFamily, DWRITE_TEXT_RANGE{static_cast<UINT32>(row.label.size()),
                                                static_cast<UINT32>(row.text.size())});
        }
        if (!row.annotation.empty() &&
            FAILED(dwriteFactory_->CreateTextLayout(
                row.annotation.c_str(), static_cast<UINT32>(row.annotation.size()), annotationFormat_,
                10000.0f, height, &row.annotationLayout))) {
            row.annotationLayout = nullptr;
        }
    }
    if (!footer_.empty()) {
        if (FAILED(dwriteFactory_->CreateTextLayout(footer_.c_str(),
                                                    static_cast<UINT32>(footer_.size()),
                                                    footerFormat_, 100.0f, height, &footerLayout_))) {
            footerLayout_ = nullptr;
        }
    }
}

SIZE CandidateWindow::MeasureContent() {
    SIZE content = {0, 0};
    const bool directWrite = !rows_.empty() && rows_.front().layout != nullptr;
    HDC dc = directWrite ? nullptr : GetDC(hwnd_);
    HGDIOBJ oldFont = dc ? SelectObject(dc, font_) : nullptr;
    LONG annotationWidth = 0;
    for (const Row& row : rows_) {
        LONG width = 0;
        LONG annotation = 0;
        DWRITE_TEXT_METRICS metrics = {};
        if (row.layout && SUCCEEDED(row.layout->GetMetrics(&metrics))) {
            width = static_cast<LONG>(std::ceil(metrics.widthIncludingTrailingWhitespace));
            if (row.annotationLayout && SUCCEEDED(row.annotationLayout->GetMetrics(&metrics))) {
                annotation = static_cast<LONG>(std::ceil(metrics.widthIncludingTrailingWhitespace));
            }
        } else if (dc) {
            const std::wstring text = row.label + row.text;
            SIZE size = {};
            GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
            width = size.cx;
            if (!row.annotation.empty()) {
                GetTextExtentPoint32W(dc, row.annotation.c_str(),
                                      static_cast<int>(row.annotation.size()), &size);
                annotation = size.cx;
            }
        }
        content.cx = (std::max)(content.cx, width);
        annotationWidth = (std::max)(annotationWidth, annotation);
    }
    if (dc) {
        SelectObject(dc, oldFont);
        ReleaseDC(hwnd_, dc);
    }
    // 印は候補の列の右に、そろえて出す
    annotationX_ = 0;
    if (annotationWidth > 0) {
        annotationX_ = padding_ + content.cx + padding_ * 3;
        content.cx += padding_ * 3 + annotationWidth;
    }
    content.cx += padding_ * 4;
    const size_t rows = rows_.size() + (footer_.empty() ? 0 : 1);
    content.cy = static_cast<LONG>(rows) * rowHeight_ + padding_ * 2;
    return content;
}

void CandidateWindow::Paint(HDC dc, const RECT& client) {
    // 背景と選んでいる行の帯は GDI で塗り、文字だけを Direct2D で重ねる
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
    if (selected_ < rows_.size()) {
        const RECT highlight = {0, padding_ + static_cast<LONG>(selected_) * rowHeight_,
                                client.right,
                                padding_ + static_cast<LONG>(selected_ + 1) * rowHeight_};
        FillRect(dc, &highlight, GetSysColorBrush(COLOR_HIGHLIGHT));
    }
    const bool hasLayouts = !rows_.empty() && rows_.front().layout != nullptr;
    if (!hasLayouts || !EnsureDirectWrite() || FAILED(renderTarget_->BindDC(dc, &client))) {
        PaintWithGdi(dc, client);
        return;
    }
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(renderTarget_->CreateSolidColorBrush(SystemColor(COLOR_WINDOWTEXT), &brush))) {
        PaintWithGdi(dc, client);
        return;
    }
    renderTarget_->BeginDraw();
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (!rows_[i].layout) continue;
        brush->SetColor(SystemColor(i == selected_ ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
        renderTarget_->DrawTextLayout(
            D2D1::Point2F(static_cast<float>(padding_),
                          static_cast<float>(padding_ + static_cast<LONG>(i) * rowHeight_)),
            rows_[i].layout, brush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        if (rows_[i].annotationLayout && annotationX_ > 0) {
            brush->SetColor(SystemColor(i == selected_ ? COLOR_HIGHLIGHTTEXT : COLOR_GRAYTEXT));
            renderTarget_->DrawTextLayout(
                D2D1::Point2F(static_cast<float>(annotationX_),
                              static_cast<float>(padding_ + static_cast<LONG>(i) * rowHeight_)),
                rows_[i].annotationLayout, brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }
    if (footerLayout_) {
        // 候補が1ページに収まらないとき、いまのページ（例: 2/7）を右下に出す
        brush->SetColor(SystemColor(COLOR_GRAYTEXT));
        const float width = static_cast<float>(client.right - padding_ * 2);
        footerLayout_->SetMaxWidth(width);
        renderTarget_->DrawTextLayout(
            D2D1::Point2F(static_cast<float>(padding_),
                          static_cast<float>(padding_ +
                                             static_cast<LONG>(rows_.size()) * rowHeight_)),
            footerLayout_, brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    const HRESULT hr = renderTarget_->EndDraw();
    brush->Release();
    // 描画の装置が失われた（画面の設定が変わったなど）: 次に描くときに作り直す
    if (hr == D2DERR_RECREATE_TARGET) SafeRelease(&renderTarget_);
}

void CandidateWindow::PaintWithGdi(HDC dc, const RECT& client) const {
    HGDIOBJ oldFont = SelectObject(dc, font_);
    SetBkMode(dc, TRANSPARENT);
    for (size_t i = 0; i < rows_.size(); ++i) {
        RECT row = {padding_, padding_ + static_cast<LONG>(i) * rowHeight_,
                    client.right - padding_,
                    padding_ + static_cast<LONG>(i + 1) * rowHeight_};
        SetTextColor(dc, GetSysColor(i == selected_ ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
        const std::wstring text = rows_[i].label + rows_[i].text;
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &row,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        if (!rows_[i].annotation.empty() && annotationX_ > 0) {
            RECT annotation = row;
            annotation.left = annotationX_;
            SetTextColor(dc, GetSysColor(i == selected_ ? COLOR_HIGHLIGHTTEXT : COLOR_GRAYTEXT));
            DrawTextW(dc, rows_[i].annotation.c_str(), static_cast<int>(rows_[i].annotation.size()),
                      &annotation, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        }
    }
    if (!footer_.empty()) {
        RECT row = {padding_, padding_ + static_cast<LONG>(rows_.size()) * rowHeight_,
                    client.right - padding_,
                    padding_ + static_cast<LONG>(rows_.size() + 1) * rowHeight_};
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextW(dc, footer_.c_str(), static_cast<int>(footer_.size()), &row,
                  DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    }
    SelectObject(dc, oldFont);
}

void CandidateWindow::Show(HWND owner, const RECT& anchor,
                           const std::vector<std::u32string>& candidates,
                           const std::vector<std::u32string>& annotations, size_t selectedIndex,
                           bool numbered) {
    EnsureWindow(owner);
    if (!hwnd_) return;
    UpdateFont();

    // 選んでいる候補のあるページだけを出す（数字キーはページの中の番号）
    const bool hasSelection = selectedIndex < candidates.size();
    const size_t page = hasSelection ? selectedIndex / kPageSize : 0;
    const size_t first = page * kPageSize;
    const size_t last = (std::min)(first + kPageSize, candidates.size());
    ReleaseLayouts();
    rows_.clear();
    for (size_t i = first; i < last; ++i) {
        Row row;
        if (numbered) row.label = std::to_wstring(i - first + 1) + L"  ";
        row.text = iroha::Utf32ToUtf16(candidates[i]);
        if (annotations.size() == candidates.size()) row.annotation = iroha::Utf32ToUtf16(annotations[i]);
        rows_.push_back(std::move(row));
    }
    selected_ = hasSelection ? selectedIndex - first : SIZE_MAX;
    const size_t pageCount = (candidates.size() + kPageSize - 1) / kPageSize;
    footer_ = pageCount > 1
                  ? std::to_wstring(page + 1) + L"/" + std::to_wstring(pageCount)
                  : std::wstring();
    BuildLayouts();

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
