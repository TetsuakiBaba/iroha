#pragma once
#include <functional>
#include <new>

#include "Globals.h"

// 関数オブジェクトを包む使い捨てのITfEditSession。
// ドキュメントの読み書きはすべてエディットセッション内でしか行えない。
class FunctionalEditSession : public ITfEditSession {
public:
    using Func = std::function<HRESULT(TfEditCookie)>;

    explicit FunctionalEditSession(Func func) : refCount_(1), func_(std::move(func)) {
        DllAddRef();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, __uuidof(ITfEditSession))) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refCount_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG count = InterlockedDecrement(&refCount_);
        if (count == 0) delete this;
        return count;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        // 例外はホストアプリを巻き込むため必ずここで止める
        try {
            return func_(ec);
        } catch (...) {
            return E_FAIL;
        }
    }

private:
    virtual ~FunctionalEditSession() { DllRelease(); }

    LONG refCount_;
    Func func_;
};

// 同期エディットセッションを要求する（キーイベント処理中は同期が許される）
inline HRESULT RequestSyncEditSession(ITfContext* context, TfClientId clientId,
                                      FunctionalEditSession::Func func,
                                      DWORD flags = TF_ES_SYNC | TF_ES_READWRITE) {
    auto* session = new (std::nothrow) FunctionalEditSession(std::move(func));
    if (!session) return E_OUTOFMEMORY;
    HRESULT hrSession = E_FAIL;
    const HRESULT hr = context->RequestEditSession(clientId, session, flags, &hrSession);
    session->Release();
    return FAILED(hr) ? hr : hrSession;
}
