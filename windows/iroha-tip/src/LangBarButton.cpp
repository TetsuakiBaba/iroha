#include "LangBarButton.h"

#include <ctffunc.h> // GUID_LBI_INPUTMODE
#include <olectl.h>  // CONNECT_E_*

#include <cstring>

#include "TextService.h"

namespace {

// タスクバーがライトテーマか（アイコンの文字色の選択に使う）
bool IsSystemLightTheme() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
                     &size) != ERROR_SUCCESS) {
        return false; // 既定はダーク
    }
    return value != 0;
}

} // namespace

LangBarButton::LangBarButton(TextService* owner) : refCount_(1), owner_(owner) {
    DllAddRef();
}

LangBarButton::~LangBarButton() {
    if (sink_) {
        sink_->Release();
        sink_ = nullptr;
    }
    DllRelease();
}

void LangBarButton::NotifyUpdate() {
    if (sink_) sink_->OnUpdate(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP);
}

STDMETHODIMP LangBarButton::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, __uuidof(ITfLangBarItem)) ||
        IsEqualIID(riid, __uuidof(ITfLangBarItemButton))) {
        *ppv = static_cast<ITfLangBarItemButton*>(this);
    } else if (IsEqualIID(riid, __uuidof(ITfSource))) {
        *ppv = static_cast<ITfSource*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) LangBarButton::AddRef() {
    return InterlockedIncrement(&refCount_);
}

STDMETHODIMP_(ULONG) LangBarButton::Release() {
    const LONG count = InterlockedDecrement(&refCount_);
    if (count == 0) delete this;
    return count;
}

STDMETHODIMP LangBarButton::GetInfo(TF_LANGBARITEMINFO* info) {
    if (!info) return E_INVALIDARG;
    info->clsidService = CLSID_IROHA_TIP;
    info->guidItem = GUID_LBI_INPUTMODE;
    info->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY;
    info->ulSort = 0;
    wcscpy_s(info->szDescription, L"iroha 入力モード");
    return S_OK;
}

STDMETHODIMP LangBarButton::GetStatus(DWORD* status) {
    if (!status) return E_INVALIDARG;
    *status = 0;
    return S_OK;
}

STDMETHODIMP LangBarButton::Show(BOOL) {
    return E_NOTIMPL;
}

STDMETHODIMP LangBarButton::GetTooltipString(BSTR* tooltip) {
    if (!tooltip) return E_INVALIDARG;
    const bool direct = owner_ && owner_->IsDirectMode();
    *tooltip = SysAllocString(direct ? L"iroha: 英数（クリックでかなに切替）"
                                     : L"iroha: かな（クリックで英数に切替）");
    return *tooltip ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP LangBarButton::OnClick(TfLBIClick, POINT, const RECT*) {
    if (owner_) owner_->OnModeButtonClicked();
    return S_OK;
}

STDMETHODIMP LangBarButton::InitMenu(ITfMenu*) {
    return E_NOTIMPL;
}

STDMETHODIMP LangBarButton::OnMenuSelect(UINT) {
    return E_NOTIMPL;
}

// 「あ」/「A」を描いたアイコンを作る（呼び出し側=システムが破棄する）
HICON LangBarButton::CreateModeIcon() const {
    const bool direct = owner_ && owner_->IsDirectMode();
    const wchar_t* text = direct ? L"A" : L"あ";
    const int size = GetSystemMetrics(SM_CXSMICON);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP color = CreateDIBSection(memory, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        DeleteDC(memory);
        return nullptr;
    }

    HGDIOBJ oldBitmap = SelectObject(memory, color);
    // 透明背景（アルファ0）に文字だけを描く。アンチエイリアスは
    // アルファ再構成が崩れるため無効にする
    std::memset(bits, 0, static_cast<size_t>(size) * size * 4);
    HFONT font = CreateFontW(-(size - 2), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Meiryo UI");
    HGDIOBJ oldFont = SelectObject(memory, font);
    SetBkMode(memory, TRANSPARENT);
    const bool light = IsSystemLightTheme();
    SetTextColor(memory, light ? RGB(0, 0, 0) : RGB(255, 255, 255));
    RECT rect = {0, 0, size, size};
    DrawTextW(memory, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    GdiFlush();

    // GDIの文字描画はアルファを書かないため、色が付いた画素を不透明にする
    auto* pixels = static_cast<DWORD*>(bits);
    for (int i = 0; i < size * size; ++i) {
        if (pixels[i] != 0) pixels[i] |= 0xFF000000;
    }

    SelectObject(memory, oldFont);
    DeleteObject(font);
    SelectObject(memory, oldBitmap);
    DeleteDC(memory);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO iconInfo = {};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = color;
    iconInfo.hbmMask = mask;
    HICON icon = CreateIconIndirect(&iconInfo);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

STDMETHODIMP LangBarButton::GetIcon(HICON* icon) {
    if (!icon) return E_INVALIDARG;
    *icon = CreateModeIcon();
    return *icon ? S_OK : E_FAIL;
}

STDMETHODIMP LangBarButton::GetText(BSTR* text) {
    if (!text) return E_INVALIDARG;
    const bool direct = owner_ && owner_->IsDirectMode();
    *text = SysAllocString(direct ? L"A" : L"あ");
    return *text ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP LangBarButton::AdviseSink(REFIID riid, IUnknown* punk, DWORD* cookie) {
    if (!punk || !cookie) return E_INVALIDARG;
    if (!IsEqualIID(riid, __uuidof(ITfLangBarItemSink))) return CONNECT_E_CANNOTCONNECT;
    if (sink_) return CONNECT_E_ADVISELIMIT;
    if (FAILED(punk->QueryInterface(IID_PPV_ARGS(&sink_)))) {
        sink_ = nullptr;
        return E_NOINTERFACE;
    }
    *cookie = 1;
    return S_OK;
}

STDMETHODIMP LangBarButton::UnadviseSink(DWORD cookie) {
    if (cookie != 1 || !sink_) return CONNECT_E_NOCONNECTION;
    sink_->Release();
    sink_ = nullptr;
    return S_OK;
}
