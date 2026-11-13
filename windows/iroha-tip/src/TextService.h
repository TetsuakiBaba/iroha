#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "CandidateWindow.h"
#include "Globals.h"
#include "iroha/input_session.h"

class LangBarButton;

// TSFテキストサービス本体。
//
// 入力の状態（読み・ライブ変換の結果・文節・候補）は変換サーバのセッションが持つ
// （iroha::InputSession）。ここで行うのは次の3つだけ:
//   1. キーコードを意味（文字・Enter・Space・矢印…）に読み替えて、受けるかどうかを決める
//   2. キーを変換サーバに送り、返ってきた表示（確定する文字列・未確定文字列・候補）を文書に反映する
//   3. ライブ変換の結果待ちの間、タイマーで変換サーバに問い合わせ、届いたら表示を更新する
// TIPはアプリのプロセスの中で動くので、判断と状態をここに置かない
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
    // プライベートモードか（言語バーのアイコンに印を付ける）
    bool IsPrivateMode() const { return privateMode_; }
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

    // このキーをどうするか。OnTestKeyDownとOnKeyDownで同じ判定を使う
    struct KeyDecision {
        enum class Kind {
            None,             // 受けない（アプリに渡す）
            Session,          // 変換サーバのセッションに送る
            CommitThenPass,   // 今の未確定文字列を確定してから、キーはアプリに渡す（Tab・Ctrl+S など）
            UndoCommit,       // 直前の確定の取り消し（カーソルの手前が確定した文字列のままのときだけ）
            ToggleDirectMode, // かな⇔英数の切替（半角/全角キー等）
            SetKanaMode,      // ひらがなキー
            SetDirectMode,    // 無変換/英数キー
        };
        Kind kind = Kind::None;
        iroha::SessionKey key = iroha::SessionKey::Character;
        char32_t character = 0;
        bool shift = false;
    };
    KeyDecision DecideKey(WPARAM wParam, LPARAM lParam) const;
    // キーを処理する。キーをアプリに渡すなら passToApp を真にする
    HRESULT HandleKey(ITfContext* context, const KeyDecision& decision, bool* passToApp);
    // カーソルの手前（選択なし）が text のままか
    bool TextBeforeCaretIs(ITfContext* context, const std::u32string& text);

    // キーを変換サーバに送り、返ってきた表示を反映する
    HRESULT SendSessionKey(ITfContext* context, iroha::SessionKey key, char32_t character,
                           bool shift);
    // 変換サーバから受け取った表示を文書に反映する。
    // synchronous が偽なら（キーの処理の外。ライブ変換が届いたとき）非同期のエディットセッションで行う
    HRESULT ApplyOutput(ITfContext* context, const iroha::SessionOutput& output, bool synchronous);
    HRESULT ApplyOutputInSession(TfEditCookie ec, ITfContext* context,
                                 const iroha::SessionOutput& output);
    HRESULT UpdateCandidateWindow(ITfContext* context);
    // 予測変換の候補・知らせを、未確定文字列（無ければカーソル）の下に出す
    HRESULT UpdatePredictionWindow(ITfContext* context);
    void ShowNotice(ITfContext* context, const std::u32string& text);
    // 未確定文字列（無ければカーソル）のスクリーン座標と、窓の持ち主
    bool CompositionRect(ITfContext* context, RECT* rect, HWND* owner);
    // カーソルより前の文書のテキストを読む（左文脈。読めなければ偽）
    bool ReadLeftContext(ITfContext* context, std::u32string* text);
    // 手元の表示の控えを捨てる（合成が終わった）
    void ClearShown();

    // ライブ変換の結果待ちの間の問い合わせ（Polling.cpp）
    void StartPolling();
    void StopPolling();
    void OnPollTimer();
    void DestroyPollWindow();
    bool EnsurePollWindow();
    static void CALLBACK PollTimerProc(HWND hwnd, UINT message, UINT_PTR id, DWORD time);
    static void CALLBACK NoticeTimerProc(HWND hwnd, UINT message, UINT_PTR id, DWORD time);

    // コンポジション操作（Composition.cpp）
    HRESULT EnsureComposition(TfEditCookie ec, ITfContext* context);
    // カーソルの手前 length 文字（UTF-16）を覆う合成を始める（確定の取り消し）
    HRESULT StartCompositionBeforeCaret(TfEditCookie ec, ITfContext* context, LONG length);
    HRESULT SetPreedit(TfEditCookie ec, ITfContext* context, const iroha::SessionOutput& output);
    void EndCompositionInternal(TfEditCookie ec, ITfContext* context,
                                const std::wstring& finalText);
    void ReleaseComposition();

    LONG refCount_;
    ITfThreadMgr* threadMgr_ = nullptr;
    TfClientId clientId_ = TF_CLIENTID_NULL;
    DWORD activateFlags_ = 0;

    ITfComposition* composition_ = nullptr;
    // 合成中の文書（ライブ変換が届いたときの反映先）
    ITfContext* compositionContext_ = nullptr;
    TfGuidAtom displayAttributeAtom_ = TF_INVALID_GUIDATOM;
    TfGuidAtom displayAttributeCurrentAtom_ = TF_INVALID_GUIDATOM;

    // 変換サーバのセッションの番号（有効化のたびに作る）
    uint64_t sessionId_ = 0;
    // 直近に変換サーバから受け取った表示。キーを受けるかの判断（文節変換中か）と、
    // 候補ウィンドウの位置合わせ、ライブ変換の問い合わせ（revision）に使う
    iroha::SessionOutput shown_;
    CandidateWindow candidateWindow_;
    // 合成していないときのキーの判断に使う、直近の返事の内容（合成が終わっても残す）:
    // 取り消せる確定（Ctrl+Backspace を受けるか）と、Space を全角にするか
    std::u32string idleUndoable_;
    bool idleFullWidthSpace_ = true;
    bool privateMode_ = false;
    // 予測変換の候補の小窓と、知らせの小窓
    CandidateWindow predictionWindow_;
    CandidateWindow noticeWindow_;
    // 確定の取り消しで、未確定文字列に置き換えるカーソルの手前の長さ（UTF-16）
    LONG replaceBeforeLength_ = 0;

    // ライブ変換の結果待ちの間だけ動かすタイマー（メッセージ専用ウィンドウに付ける）
    HWND pollWindow_ = nullptr;
    bool polling_ = false;
    DWORD pollStartedTick_ = 0;
    int pollFailures_ = 0;

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
};
