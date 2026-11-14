#include "LangBarButton.h"

#include <ctffunc.h> // GUID_LBI_INPUTMODE
#include <olectl.h>  // CONNECT_E_*
#include <shellapi.h>

#include <cstring>
#include <string>

#include "TextService.h"
#include "key_shortcut.h"

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
    // TEXTCOLORICON: モノクロのアイコンをホストがテーマの文字色で塗る。
    // BTN_BUTTON: 左クリック・右クリックとも OnClick に届く（左は切り替え、右はメニュー。2026-11-14 実機で確かめた形）
    info->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY | TF_LBI_STYLE_TEXTCOLORICON;
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
    const bool privateMode = owner_ && owner_->IsPrivateMode();
    std::wstring text = direct ? L"iroha: 半角英数" : L"iroha: ひらがな";
    std::wstring modeKey;
    if (owner_ && owner_->InputModeShortcut() != 0) {
        const std::string key = iroha::KeyShortcut::Decode(owner_->InputModeShortcut()).Text();
        modeKey = L"・" + std::wstring(key.begin(), key.end());
    }
    text += L"\nクリック" + modeKey + L"で切り替え、右クリックでメニュー（設定など）";
    if (privateMode) {
        const uint32_t shortcut = owner_->PrivateShortcut();
        text += L"\nプライベートモード";
        if (shortcut != 0) {
            const std::string key = iroha::KeyShortcut::Decode(shortcut).Text();
            text += L"（" + std::wstring(key.begin(), key.end()) + L" で戻す）";
        }
    }
    *tooltip = SysAllocString(text.c_str());
    return *tooltip ? S_OK : E_OUTOFMEMORY;
}

// 左クリックはかな・英数の切り替え、右クリックはメニュー（Windows の IME と同じ。項目は TextService::MenuEntries）
STDMETHODIMP LangBarButton::OnClick(TfLBIClick click, POINT point, const RECT*) {
    if (!owner_) return S_OK;
    if (click != TF_LBI_CLK_RIGHT) {
        owner_->OnModeButtonClicked();
        return S_OK;
    }
    HMENU menu = CreatePopupMenu();
    if (!menu) return S_OK;
    for (const TextService::MenuEntry& entry : owner_->MenuEntries()) {
        if (entry.id == 0) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        UINT flags = MF_STRING | (entry.checked ? MF_CHECKED : 0) | (entry.grayed ? MF_GRAYED : 0);
        AppendMenuW(menu, flags, entry.id, entry.text.c_str());
        if (entry.radio && entry.checked) CheckMenuRadioItem(menu, entry.id, entry.id, entry.id, MF_BYCOMMAND);
    }
    // メニューの持ち主にする見えない窓（このスレッドの窓が要る）。前面にしておかないと、メニューの外を押しても閉じない
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, point.x, point.y, 0, 0, nullptr, nullptr,
                                 g_hInst, nullptr);
    UINT chosen = 0;
    if (owner) {
        SetForegroundWindow(owner);
        chosen = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, point.x,
                                  point.y, owner, nullptr);
        PostMessageW(owner, WM_NULL, 0, 0);
        DestroyWindow(owner);
    }
    DestroyMenu(menu);
    if (chosen != 0 && owner_) owner_->OnMenuCommand(chosen);
    return S_OK;
}

// メニューの形のボタンにしたとき・言語バーがメニューを求めたときのために、同じ項目を渡す
STDMETHODIMP LangBarButton::InitMenu(ITfMenu* menu) {
    if (!menu || !owner_) return E_INVALIDARG;
    for (const TextService::MenuEntry& entry : owner_->MenuEntries()) {
        if (entry.id == 0) {
            menu->AddMenuItem(0, TF_LBMENUF_SEPARATOR, nullptr, nullptr, nullptr, 0, nullptr);
            continue;
        }
        DWORD flags = 0;
        if (entry.checked) flags |= entry.radio ? TF_LBMENUF_RADIOCHECKED : TF_LBMENUF_CHECKED;
        if (entry.grayed) flags |= TF_LBMENUF_GRAYED;
        menu->AddMenuItem(entry.id, flags, nullptr, nullptr, entry.text.c_str(), static_cast<ULONG>(entry.text.size()),
                          nullptr);
    }
    return S_OK;
}

STDMETHODIMP LangBarButton::OnMenuSelect(UINT id) {
    if (owner_) owner_->OnMenuCommand(id);
    return S_OK;
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
    // TF_LBI_STYLE_TEXTCOLORICONの場合ホストがテーマ色で塗り直すため
    // 白で描く（RGB(0,0,0)は透過画素と区別できないため使わない）。
    // ホストが塗らない環境向けのフォールバックとしてテーマに合わせる
    const bool light = IsSystemLightTheme();
    SetTextColor(memory, light ? RGB(16, 16, 16) : RGB(255, 255, 255));
    RECT rect = {0, 0, size, size};
    DrawTextW(memory, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    // プライベートモードの間は下に線を引く（macOS版はカーソルの横に鍵のマークを出す）
    if (owner_ && owner_->IsPrivateMode()) {
        const int thickness = size >= 24 ? 3 : 2;
        RECT bar = {1, size - thickness, size - 1, size};
        HBRUSH brush = CreateSolidBrush(light ? RGB(16, 16, 16) : RGB(255, 255, 255));
        FillRect(memory, &bar, brush);
        DeleteObject(brush);
    }
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
