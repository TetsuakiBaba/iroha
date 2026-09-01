#pragma once
#include "Globals.h"

class TextService;

// 通知領域（言語バー）の入力モードボタン。
// 「あ」（かな）/「A」（英数）のアイコンを表示し、クリックでモードを切り替える。
// アイコンは実行時にGDIで描画する（タスクバーのテーマに合わせて白黒を選ぶ）。
class LangBarButton : public ITfLangBarItemButton, public ITfSource {
public:
    explicit LangBarButton(TextService* owner);

    // TextServiceの解放時に呼ぶ（以後ownerには触らない）
    void Detach() { owner_ = nullptr; }
    // モードが変わったときにアイコンの再描画を促す
    void NotifyUpdate();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfLangBarItem
    STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* info) override;
    STDMETHODIMP GetStatus(DWORD* status) override;
    STDMETHODIMP Show(BOOL show) override;
    STDMETHODIMP GetTooltipString(BSTR* tooltip) override;

    // ITfLangBarItemButton
    STDMETHODIMP OnClick(TfLBIClick click, POINT pt, const RECT* area) override;
    STDMETHODIMP InitMenu(ITfMenu* menu) override;
    STDMETHODIMP OnMenuSelect(UINT id) override;
    STDMETHODIMP GetIcon(HICON* icon) override;
    STDMETHODIMP GetText(BSTR* text) override;

    // ITfSource
    STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD* cookie) override;
    STDMETHODIMP UnadviseSink(DWORD cookie) override;

private:
    virtual ~LangBarButton();
    HICON CreateModeIcon() const;

    LONG refCount_;
    TextService* owner_;
    ITfLangBarItemSink* sink_ = nullptr;
};
