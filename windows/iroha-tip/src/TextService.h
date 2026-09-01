#pragma once
#include <string>
#include <vector>

#include "CandidateWindow.h"
#include "Globals.h"
#include "iroha/romaji_composer.h"

// TSFテキストサービス本体。
// M1: 登録・有効化。M2/M3: キー入力→ローマ字かな合成のコンポジション表示・確定。
// M4: Spaceで変換サーバに問い合わせ、Space/↑↓で候補送り（候補ウィンドウはM4.5）。
class TextService : public ITfTextInputProcessorEx,
                    public ITfKeyEventSink,
                    public ITfCompositionSink,
                    public ITfDisplayAttributeProvider {
public:
    TextService();
    TextService(const TextService&) = delete;
    TextService& operator=(const TextService&) = delete;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfTextInputProcessor
    STDMETHODIMP Activate(ITfThreadMgr* threadMgr, TfClientId clientId) override;
    STDMETHODIMP Deactivate() override;

    // ITfTextInputProcessorEx
    STDMETHODIMP ActivateEx(ITfThreadMgr* threadMgr, TfClientId clientId, DWORD flags) override;

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL foreground) override;
    STDMETHODIMP OnTestKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) override;
    STDMETHODIMP OnKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext* context, REFGUID rguid, BOOL* eaten) override;

    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* composition) override;

    // ITfDisplayAttributeProvider
    STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** enumInfo) override;
    STDMETHODIMP GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** info) override;

private:
    ~TextService();

    // このキーで行う操作。OnTestKeyDownとOnKeyDownで同じ判定を使う
    enum class KeyAction {
        None,            // 食わない（ホストに渡す）
        Input,           // 文字としてコンポーザへ
        Commit,          // 確定（変換中は現候補、入力中はかな）
        Cancel,          // 破棄
        Backspace,       // 表示上の1文字削除
        Convert,         // 変換サーバに問い合わせて候補表示へ
        NextCandidate,   // 次候補
        PrevCandidate,   // 前候補
        SelectCandidate, // 数字キーで候補を直接選択
        BackToComposing, // 候補表示をやめて読みに戻る
        CommitThenInput, // 現候補を確定して新しい入力を始める
        ToggleDirectMode, // かな⇔英数の切替（半角/全角キー等）
        SetKanaMode,      // ひらがなキー
        SetDirectMode,    // 無変換/英数キー
    };
    KeyAction DecideKeyAction(WPARAM wParam, LPARAM lParam, wchar_t* outChar) const;
    HRESULT HandleKey(ITfContext* context, KeyAction action, wchar_t character);
    HRESULT StartConversion(ITfContext* context);
    HRESULT ShowCurrentCandidate(ITfContext* context); // インライン表示+候補ウィンドウ更新
    HRESULT UpdateCandidateWindow(ITfContext* context);
    std::u32string ReadLeftContext(ITfContext* context); // コンポジション直前の文書テキスト

    // コンポジション操作（Composition.cpp）
    HRESULT ShowText(ITfContext* context, const std::wstring& text);
    HRESULT UpdateComposition(ITfContext* context); // コンポーザの表示文字列を出す
    HRESULT CommitText(ITfContext* context, const std::wstring& text);
    HRESULT CancelComposition(ITfContext* context);
    HRESULT EnsureComposition(TfEditCookie ec, ITfContext* context);
    HRESULT SetCompositionText(TfEditCookie ec, ITfContext* context,
                               const std::wstring& text, bool underline);
    void EndCompositionInternal(TfEditCookie ec, ITfContext* context,
                                const std::wstring& finalText);
    void ResetState();

    LONG refCount_;
    ITfThreadMgr* threadMgr_ = nullptr;
    TfClientId clientId_ = TF_CLIENTID_NULL;
    DWORD activateFlags_ = 0;

    ITfComposition* composition_ = nullptr;
    iroha::RomajiComposer composer_;
    TfGuidAtom displayAttributeAtom_ = TF_INVALID_GUIDATOM;

    // 英数モード（trueの間はキーを一切食わない）。半角/全角キー等で切り替える
    bool directMode_ = false;
    // 変換状態（M4）。convertingの間、コンポジションには現候補を表示する
    bool converting_ = false;
    std::vector<std::u32string> candidates_;
    size_t candidateIndex_ = 0;
    CandidateWindow candidateWindow_;
};
