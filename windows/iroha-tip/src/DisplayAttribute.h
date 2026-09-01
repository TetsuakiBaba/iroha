#pragma once
#include "Globals.h"

// 未確定文字列の表示属性（下線）。
// ITfDisplayAttributeProvider（TextService側で実装）がこの1件を返し、
// アプリはこれを引いてコンポジションの見た目を描画する。
class DisplayAttributeInfo : public ITfDisplayAttributeInfo {
public:
    DisplayAttributeInfo();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfDisplayAttributeInfo
    STDMETHODIMP GetGUID(GUID* guid) override;
    STDMETHODIMP GetDescription(BSTR* description) override;
    STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE* attribute) override;
    STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE* attribute) override;
    STDMETHODIMP Reset() override;

private:
    virtual ~DisplayAttributeInfo();
    LONG refCount_;
};

// 表示属性1件だけを列挙するIEnumTfDisplayAttributeInfo
class EnumDisplayAttributeInfoImpl : public IEnumTfDisplayAttributeInfo {
public:
    EnumDisplayAttributeInfoImpl();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IEnumTfDisplayAttributeInfo
    STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo** enumInfo) override;
    STDMETHODIMP Next(ULONG count, ITfDisplayAttributeInfo** info, ULONG* fetched) override;
    STDMETHODIMP Reset() override;
    STDMETHODIMP Skip(ULONG count) override;

private:
    virtual ~EnumDisplayAttributeInfoImpl();
    LONG refCount_;
    ULONG index_ = 0;
};
