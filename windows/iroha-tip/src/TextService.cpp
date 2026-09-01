#include "TextService.h"

#include <new>

#include "DisplayAttribute.h"

TextService::TextService() : refCount_(1) { DllAddRef(); }

TextService::~TextService() {
    if (composition_) {
        composition_->Release();
        composition_ = nullptr;
    }
    if (threadMgr_) {
        threadMgr_->Release();
        threadMgr_ = nullptr;
    }
    DllRelease();
}

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, __uuidof(ITfTextInputProcessor))) {
        *ppv = static_cast<ITfTextInputProcessor*>(this);
    } else if (IsEqualIID(riid, __uuidof(ITfTextInputProcessorEx))) {
        *ppv = static_cast<ITfTextInputProcessorEx*>(this);
    } else if (IsEqualIID(riid, __uuidof(ITfKeyEventSink))) {
        *ppv = static_cast<ITfKeyEventSink*>(this);
    } else if (IsEqualIID(riid, __uuidof(ITfCompositionSink))) {
        *ppv = static_cast<ITfCompositionSink*>(this);
    } else if (IsEqualIID(riid, __uuidof(ITfDisplayAttributeProvider))) {
        *ppv = static_cast<ITfDisplayAttributeProvider*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) TextService::AddRef() {
    return InterlockedIncrement(&refCount_);
}

STDMETHODIMP_(ULONG) TextService::Release() {
    const LONG count = InterlockedDecrement(&refCount_);
    if (count == 0) delete this;
    return count;
}

STDMETHODIMP TextService::Activate(ITfThreadMgr* threadMgr, TfClientId clientId) {
    return ActivateEx(threadMgr, clientId, 0);
}

STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* threadMgr, TfClientId clientId,
                                     DWORD flags) {
    IrohaLog(L"ActivateEx clientId=%u flags=0x%08X", clientId, flags);
    if (!threadMgr) return E_INVALIDARG;

    threadMgr_ = threadMgr;
    threadMgr_->AddRef();
    clientId_ = clientId;
    activateFlags_ = flags;

    // キーイベントシンク（foreground=TRUE: このTIP選択中のキーを受ける）
    ITfKeystrokeMgr* keystrokeMgr = nullptr;
    HRESULT hr = threadMgr_->QueryInterface(IID_PPV_ARGS(&keystrokeMgr));
    if (SUCCEEDED(hr)) {
        hr = keystrokeMgr->AdviseKeyEventSink(
            clientId_, static_cast<ITfKeyEventSink*>(this), TRUE);
        keystrokeMgr->Release();
    }
    if (FAILED(hr)) {
        IrohaLog(L"AdviseKeyEventSink failed hr=0x%08X", hr);
        Deactivate();
        return hr;
    }

    // 表示属性GUIDをatom化（コンポジションのレンジに下線属性を付けるのに使う）
    ITfCategoryMgr* categoryMgr = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&categoryMgr)))) {
        categoryMgr->RegisterGUID(GUID_IROHA_DISPLAY_ATTRIBUTE, &displayAttributeAtom_);
        categoryMgr->Release();
    }
    return S_OK;
}

STDMETHODIMP TextService::Deactivate() {
    IrohaLog(L"Deactivate");
    if (threadMgr_) {
        ITfKeystrokeMgr* keystrokeMgr = nullptr;
        if (SUCCEEDED(threadMgr_->QueryInterface(IID_PPV_ARGS(&keystrokeMgr)))) {
            keystrokeMgr->UnadviseKeyEventSink(clientId_);
            keystrokeMgr->Release();
        }
        threadMgr_->Release();
        threadMgr_ = nullptr;
    }
    // コンポジションはエディットセッションなしでは閉じられないため参照だけ手放す
    if (composition_) {
        composition_->Release();
        composition_ = nullptr;
    }
    composer_.Clear();
    clientId_ = TF_CLIENTID_NULL;
    return S_OK;
}

// ---- ITfKeyEventSink ----

namespace {

// VKコード+キーボード状態を文字にする（現在のレイアウトに従う）
wchar_t CharFromKey(WPARAM wParam, LPARAM lParam) {
    BYTE state[256];
    if (!GetKeyboardState(state)) return 0;
    wchar_t buf[4] = {};
    const UINT scanCode = (lParam >> 16) & 0xFF;
    const int n =
        ToUnicode(static_cast<UINT>(wParam), scanCode, state, buf, ARRAYSIZE(buf), 0);
    return n == 1 ? buf[0] : 0;
}

bool IsComposerSymbol(wchar_t c) {
    // RomajiComposerの記号テーブルにあるキー + n' 用のアポストロフィ
    return c == L'-' || c == L',' || c == L'.' || c == L'/' || c == L'[' ||
           c == L']' || c == L'!' || c == L'?' || c == L'~' || c == L'\'';
}

} // namespace

TextService::KeyAction TextService::DecideKeyAction(WPARAM wParam, LPARAM lParam,
                                                    wchar_t* outChar) const {
    *outChar = 0;
    // Ctrl/Altショートカットには介入しない
    if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) {
        return KeyAction::None;
    }
    const bool composing = composition_ != nullptr;
    switch (wParam) {
        case VK_RETURN:
            return composing ? KeyAction::Commit : KeyAction::None;
        case VK_ESCAPE:
            return composing ? KeyAction::Cancel : KeyAction::None;
        case VK_BACK:
            return composing ? KeyAction::Backspace : KeyAction::None;
        case VK_SPACE:
            // M4: ここを変換サーバへの問い合わせに置き換える
            return composing ? KeyAction::Commit : KeyAction::None;
        default:
            break;
    }
    const wchar_t c = CharFromKey(wParam, lParam);
    if (c == 0) return KeyAction::None;
    const bool isLetter = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
    const bool isDigit = (c >= L'0' && c <= L'9');
    if (isLetter || IsComposerSymbol(c) || (composing && isDigit)) {
        *outChar = c;
        return KeyAction::Input;
    }
    return KeyAction::None;
}

HRESULT TextService::HandleKey(ITfContext* context, KeyAction action,
                               wchar_t character) {
    switch (action) {
        case KeyAction::Input:
            composer_.Input(static_cast<char32_t>(character));
            return UpdateComposition(context);
        case KeyAction::Backspace:
            composer_.DeleteBackward();
            if (composer_.Empty()) return CancelComposition(context);
            return UpdateComposition(context);
        case KeyAction::Commit:
            composer_.Flush();
            return CommitComposition(context);
        case KeyAction::Cancel:
            return CancelComposition(context);
        case KeyAction::None:
            break;
    }
    return S_OK;
}

STDMETHODIMP TextService::OnSetFocus(BOOL) {
    return S_OK;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* context, WPARAM wParam,
                                        LPARAM lParam, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    wchar_t c = 0;
    *eaten = (context != nullptr &&
              DecideKeyAction(wParam, lParam, &c) != KeyAction::None);
    return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam,
                                    BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    if (!context) return S_OK;
    wchar_t c = 0;
    const KeyAction action = DecideKeyAction(wParam, lParam, &c);
    if (action == KeyAction::None) return S_OK;
    *eaten = TRUE;
    const HRESULT hr = HandleKey(context, action, c);
    if (FAILED(hr)) IrohaLog(L"HandleKey failed action=%d hr=0x%08X", action, hr);
    return S_OK;
}

STDMETHODIMP TextService::OnTestKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    return S_OK;
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext*, REFGUID, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    return S_OK;
}

// ---- ITfCompositionSink ----

STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie,
                                                  ITfComposition* composition) {
    // アプリ側都合（フォーカス移動等）で強制終了された。
    // 表示中の文字列はそのままドキュメントに残るので、内部状態だけ捨てる。
    IrohaLog(L"OnCompositionTerminated");
    if (composition_ == composition && composition_) {
        composition_->Release();
        composition_ = nullptr;
    }
    composer_.Clear();
    return S_OK;
}

// ---- ITfDisplayAttributeProvider ----

STDMETHODIMP TextService::EnumDisplayAttributeInfo(
    IEnumTfDisplayAttributeInfo** enumInfo) {
    if (!enumInfo) return E_INVALIDARG;
    auto* impl = new (std::nothrow) EnumDisplayAttributeInfoImpl();
    if (!impl) return E_OUTOFMEMORY;
    *enumInfo = impl;
    return S_OK;
}

STDMETHODIMP TextService::GetDisplayAttributeInfo(REFGUID guid,
                                                  ITfDisplayAttributeInfo** info) {
    if (!info) return E_INVALIDARG;
    *info = nullptr;
    if (!IsEqualGUID(guid, GUID_IROHA_DISPLAY_ATTRIBUTE)) return E_INVALIDARG;
    auto* impl = new (std::nothrow) DisplayAttributeInfo();
    if (!impl) return E_OUTOFMEMORY;
    *info = impl;
    return S_OK;
}
