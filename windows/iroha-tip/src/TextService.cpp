#include "TextService.h"

TextService::TextService() : refCount_(1) { DllAddRef(); }

TextService::~TextService() {
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

    // M2: ITfKeystrokeMgr::AdviseKeyEventSink / ITfThreadMgrEventSink をここで登録する
    return S_OK;
}

STDMETHODIMP TextService::Deactivate() {
    IrohaLog(L"Deactivate");
    if (threadMgr_) {
        threadMgr_->Release();
        threadMgr_ = nullptr;
    }
    clientId_ = TF_CLIENTID_NULL;
    return S_OK;
}
