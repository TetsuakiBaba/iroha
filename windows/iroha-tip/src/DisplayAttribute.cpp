#include "DisplayAttribute.h"

#include <new>

// ---- DisplayAttributeInfo ----

DisplayAttributeInfo::DisplayAttributeInfo() : refCount_(1) { DllAddRef(); }
DisplayAttributeInfo::~DisplayAttributeInfo() { DllRelease(); }

STDMETHODIMP DisplayAttributeInfo::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, __uuidof(ITfDisplayAttributeInfo))) {
        *ppv = static_cast<ITfDisplayAttributeInfo*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) DisplayAttributeInfo::AddRef() {
    return InterlockedIncrement(&refCount_);
}

STDMETHODIMP_(ULONG) DisplayAttributeInfo::Release() {
    const LONG count = InterlockedDecrement(&refCount_);
    if (count == 0) delete this;
    return count;
}

STDMETHODIMP DisplayAttributeInfo::GetGUID(GUID* guid) {
    if (!guid) return E_INVALIDARG;
    *guid = GUID_IROHA_DISPLAY_ATTRIBUTE;
    return S_OK;
}

STDMETHODIMP DisplayAttributeInfo::GetDescription(BSTR* description) {
    if (!description) return E_INVALIDARG;
    *description = SysAllocString(L"iroha composition");
    return *description ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP DisplayAttributeInfo::GetAttributeInfo(TF_DISPLAYATTRIBUTE* attribute) {
    if (!attribute) return E_INVALIDARG;
    *attribute = {};
    attribute->crText.type = TF_CT_NONE;
    attribute->crBk.type = TF_CT_NONE;
    attribute->crLine.type = TF_CT_NONE;
    attribute->lsStyle = TF_LS_SOLID; // 実線の下線
    attribute->fBoldLine = FALSE;
    attribute->bAttr = TF_ATTR_INPUT;
    return S_OK;
}

STDMETHODIMP DisplayAttributeInfo::SetAttributeInfo(const TF_DISPLAYATTRIBUTE*) {
    return E_NOTIMPL;
}

STDMETHODIMP DisplayAttributeInfo::Reset() {
    return S_OK;
}

// ---- EnumDisplayAttributeInfoImpl ----

EnumDisplayAttributeInfoImpl::EnumDisplayAttributeInfoImpl() : refCount_(1) {
    DllAddRef();
}
EnumDisplayAttributeInfoImpl::~EnumDisplayAttributeInfoImpl() { DllRelease(); }

STDMETHODIMP EnumDisplayAttributeInfoImpl::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, __uuidof(IEnumTfDisplayAttributeInfo))) {
        *ppv = static_cast<IEnumTfDisplayAttributeInfo*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) EnumDisplayAttributeInfoImpl::AddRef() {
    return InterlockedIncrement(&refCount_);
}

STDMETHODIMP_(ULONG) EnumDisplayAttributeInfoImpl::Release() {
    const LONG count = InterlockedDecrement(&refCount_);
    if (count == 0) delete this;
    return count;
}

STDMETHODIMP EnumDisplayAttributeInfoImpl::Clone(IEnumTfDisplayAttributeInfo** enumInfo) {
    if (!enumInfo) return E_INVALIDARG;
    auto* clone = new (std::nothrow) EnumDisplayAttributeInfoImpl();
    if (!clone) return E_OUTOFMEMORY;
    clone->index_ = index_;
    *enumInfo = clone;
    return S_OK;
}

STDMETHODIMP EnumDisplayAttributeInfoImpl::Next(ULONG count,
                                                ITfDisplayAttributeInfo** info,
                                                ULONG* fetched) {
    if (!info) return E_INVALIDARG;
    ULONG got = 0;
    if (count > 0 && index_ == 0) {
        auto* item = new (std::nothrow) DisplayAttributeInfo();
        if (!item) return E_OUTOFMEMORY;
        info[0] = item;
        got = 1;
        index_ = 1;
    }
    if (fetched) *fetched = got;
    return got == count ? S_OK : S_FALSE;
}

STDMETHODIMP EnumDisplayAttributeInfoImpl::Reset() {
    index_ = 0;
    return S_OK;
}

STDMETHODIMP EnumDisplayAttributeInfoImpl::Skip(ULONG count) {
    index_ += count;
    return index_ <= 1 ? S_OK : S_FALSE;
}
