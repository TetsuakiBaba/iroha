#pragma once
#include "Globals.h"

// 未確定文字列の表示属性。2種類を提供する:
//   GUID_IROHA_DISPLAY_ATTRIBUTE         — 通常の下線（入力中・非選択の文節）
//   GUID_IROHA_DISPLAY_ATTRIBUTE_CURRENT — 太い下線（選択中の文節）
// ITfDisplayAttributeProvider（TextService側で実装）がこれらを返し、
// アプリはこれを引いてコンポジションの見た目を描画する。
class DisplayAttributeInfo : public ITfDisplayAttributeInfo {
public:
    // boldLine: 太下線（選択中の文節用）にするか
    explicit DisplayAttributeInfo(bool boldLine);

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
    bool boldLine_;
};

// 表示属性2件を列挙するIEnumTfDisplayAttributeInfo
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
    static constexpr ULONG kCount = 2;
    virtual ~EnumDisplayAttributeInfoImpl();
    LONG refCount_;
    ULONG index_ = 0;
};
