#pragma once
#include <string>
#include <vector>

#include "CandidateWindow.h"
#include "Globals.h"
#include "iroha/romaji_composer.h"

class LangBarButton;

// TSFテキストサービス本体。
// M1: 登録・有効化。M2/M3: キー入力→ローマ字かな合成のコンポジション表示・確定。
// M4: Spaceで変換して文節モードへ。←→で文節移動、Shift+←→で区切り調整、
//     Space/↑↓で選択中の文節の候補送り（候補ウィンドウ）、Enterで一括確定。
class TextService : public ITfTextInputProcessorEx,
                    public ITfKeyEventSink,
                    public ITfCompositionSink,
                    public ITfDisplayAttributeProvider,
                    public ITfCompartmentEventSink {
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

    // ITfCompartmentEventSink（入力モードのコンパートメント変更の監視）
    STDMETHODIMP OnChange(REFGUID rguid) override;

    // 通知領域のモードボタンから呼ばれる
    bool IsDirectMode() const { return directMode_; }
    void OnModeButtonClicked();

private:
    // 入力モード（かな/英数）の切替を1箇所に集約する。
    // タスクバーの入力インジケーターはコンパートメント値を見て「あ/A」を描くため、
    // モード変更は必ずコンパートメントにも反映する
    void SetDirectModeInternal(bool direct);
    void ApplyModeToCompartments();
    HRESULT InitCompartments();
    void ReleaseCompartments();
    ~TextService();

    // このキーで行う操作。OnTestKeyDownとOnKeyDownで同じ判定を使う
    enum class KeyAction {
        None,             // 食わない（ホストに渡す）
        Input,            // 文字としてコンポーザへ
        Commit,           // 確定（文節モードなら全文節、入力中はかな）
        Cancel,           // 破棄
        Backspace,        // 表示上の1文字削除
        Convert,          // 変換サーバに問い合わせて文節モードへ
        NextCandidate,    // 選択中の文節の次候補（候補ウィンドウを開く）
        PrevCandidate,    // 前候補
        SelectCandidate,  // 数字キーで候補を直接選択
        BackToComposing,  // 変換をやめて読みに戻る
        CommitThenInput,  // 全文節を確定して新しい入力を始める
        MoveSegmentLeft,  // 選択文節を左へ
        MoveSegmentRight, // 選択文節を右へ
        ShrinkSegment,    // 選択文節の読みを1文字縮める（Shift+←）
        ExtendSegment,    // 選択文節の読みを1文字伸ばす（Shift+→）
        ToggleDirectMode, // かな⇔英数の切替（半角/全角キー等）
        SetKanaMode,      // ひらがなキー
        SetDirectMode,    // 無変換/英数キー
    };
    KeyAction DecideKeyAction(WPARAM wParam, LPARAM lParam, wchar_t* outChar) const;
    HRESULT HandleKey(ITfContext* context, KeyAction action, wchar_t character);
    HRESULT StartConversion(ITfContext* context);
    // 現在の内容（文節モードなら全文節の結合、入力中ならかな）を確定し、
    // 変換確定ならサーバに学習用の通知を送る
    HRESULT CommitCurrent(ITfContext* context);
    HRESULT CycleCandidate(ITfContext* context, int delta);
    HRESULT SelectCandidateIndex(ITfContext* context, size_t index);
    HRESULT EnsureSegmentCandidates(ITfContext* context, size_t segmentIndex);
    HRESULT MoveSegment(ITfContext* context, int delta);
    HRESULT ResizeSegment(ITfContext* context, int delta);
    HRESULT UpdateCandidateWindow(ITfContext* context);
    std::u32string ReadLeftContext(ITfContext* context); // コンポジション直前の文書テキスト
    std::u32string JoinedResults() const;
    std::u32string LeftResults(size_t segmentIndex) const; // 選択文節より左の結果の結合

    // コンポジション操作（Composition.cpp）
    HRESULT ShowText(ITfContext* context, const std::wstring& text);
    HRESULT UpdateComposition(ITfContext* context); // コンポーザの表示文字列を出す
    HRESULT RefreshSegmentDisplay(ITfContext* context); // 文節列の表示（選択文節は太下線）
    HRESULT CommitText(ITfContext* context, const std::wstring& text);
    HRESULT CancelComposition(ITfContext* context);
    HRESULT EnsureComposition(TfEditCookie ec, ITfContext* context);
    HRESULT SetCompositionText(TfEditCookie ec, ITfContext* context,
                               const std::wstring& text, bool underline);
    HRESULT SetSegmentedText(TfEditCookie ec, ITfContext* context);
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
    TfGuidAtom displayAttributeCurrentAtom_ = TF_INVALID_GUIDATOM;

    // 英数モード（trueの間はキーを一切食わない）。半角/全角キー等で切り替える
    bool directMode_ = false;
    // 通知領域の入力モードボタン（あ/A）
    LangBarButton* langBarButton_ = nullptr;
    // 入力モードのコンパートメント（タスクバーの入力インジケーターが参照する）
    ITfCompartment* openCloseCompartment_ = nullptr;
    ITfCompartment* conversionModeCompartment_ = nullptr;
    DWORD openCloseCookie_ = TF_INVALID_COOKIE;
    DWORD conversionModeCookie_ = TF_INVALID_COOKIE;
    bool updatingCompartments_ = false; // 自分の書き込みによるOnChangeを無視する

    // 変換後の文節（Spaceで変換すると入力全体がこの列になる）
    struct ConversionSegment {
        std::u32string reading; // ひらがな
        std::u32string result;  // 現在選ばれている変換結果
        std::vector<std::u32string> candidates; // 未取得なら空
        size_t candidateIndex = 0;
    };
    // segmented_の間、コンポジションにはsegments_の結果を表示する
    bool segmented_ = false;
    std::vector<ConversionSegment> segments_;
    size_t currentSegment_ = 0;
    bool candidateListOpen_ = false; // 候補ウィンドウを開いているか
    CandidateWindow candidateWindow_;
    // 学習用: 変換した読み全体とエンジンの第一候補（確定時にサーバへ通知する）
    std::u32string conversionReading_;
    std::u32string conversionBaseline_;
};
