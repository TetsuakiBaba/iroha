#include "TextService.h"

#include <cstring>
#include <memory>
#include <new>

#include "ConvertClient.h"
#include "DisplayAttribute.h"
#include "EditSession.h"
#include "LangBarButton.h"
#include "iroha/unicode.h"

TextService::TextService() : refCount_(1) { DllAddRef(); }

TextService::~TextService() {
    DestroyPollWindow();
    ReleaseComposition();
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
    } else if (IsEqualIID(riid, __uuidof(ITfCompartmentEventSink))) {
        *ppv = static_cast<ITfCompartmentEventSink*>(this);
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
        categoryMgr->RegisterGUID(GUID_IROHA_DISPLAY_ATTRIBUTE_CURRENT,
                                  &displayAttributeCurrentAtom_);
        categoryMgr->Release();
    }

    // 通知領域の入力モードボタン（あ/A）
    ITfLangBarItemMgr* langBarMgr = nullptr;
    if (SUCCEEDED(threadMgr_->QueryInterface(IID_PPV_ARGS(&langBarMgr)))) {
        langBarButton_ = new (std::nothrow) LangBarButton(this);
        if (langBarButton_ && FAILED(langBarMgr->AddItem(langBarButton_))) {
            langBarButton_->Release();
            langBarButton_ = nullptr;
        }
        langBarMgr->Release();
    }

    // 入力モードのコンパートメント（タスクバーの「あ/A」表示の実体）
    InitCompartments();
    ApplyModeToCompartments();

    // 変換サーバのセッションの番号。ほかの TIP（別のアプリ・スレッド）と重ならない値にする
    GUID guid = {};
    if (SUCCEEDED(CoCreateGuid(&guid))) {
        memcpy(&sessionId_, &guid, sizeof(sessionId_));
    } else {
        sessionId_ = (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
                     (static_cast<uint64_t>(GetCurrentThreadId()) << 16) ^ GetTickCount64();
    }

    // 変換サーバを先に起こしてモデルのプリロードを走らせておく
    ConvertClient::EnsureServer();
    return S_OK;
}

namespace {

// IMEオン（ひらがな・ローマ字入力）の変換モード値
constexpr DWORD kKanaConversionMode =
    TF_CONVERSIONMODE_NATIVE | TF_CONVERSIONMODE_FULLSHAPE | TF_CONVERSIONMODE_ROMAN;

HRESULT SetCompartmentDword(ITfCompartment* compartment, TfClientId clientId,
                            DWORD value) {
    if (!compartment) return E_FAIL;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_I4;
    var.lVal = static_cast<LONG>(value);
    return compartment->SetValue(clientId, &var);
}

DWORD GetCompartmentDword(ITfCompartment* compartment, DWORD fallback) {
    if (!compartment) return fallback;
    VARIANT var;
    VariantInit(&var);
    if (FAILED(compartment->GetValue(&var)) || var.vt != VT_I4) return fallback;
    return static_cast<DWORD>(var.lVal);
}

} // namespace

HRESULT TextService::InitCompartments() {
    ITfCompartmentMgr* mgr = nullptr;
    HRESULT hr = threadMgr_->QueryInterface(IID_PPV_ARGS(&mgr));
    if (FAILED(hr)) return hr;
    mgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &openCloseCompartment_);
    mgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION,
                        &conversionModeCompartment_);
    mgr->Release();

    // 外部（タスクバーの入力インジケーターのクリック等）からの変更を監視する
    auto advise = [this](ITfCompartment* compartment, DWORD* cookie) {
        if (!compartment) return;
        ITfSource* source = nullptr;
        if (SUCCEEDED(compartment->QueryInterface(IID_PPV_ARGS(&source)))) {
            source->AdviseSink(__uuidof(ITfCompartmentEventSink),
                               static_cast<ITfCompartmentEventSink*>(this), cookie);
            source->Release();
        }
    };
    advise(openCloseCompartment_, &openCloseCookie_);
    advise(conversionModeCompartment_, &conversionModeCookie_);
    return S_OK;
}

void TextService::ReleaseCompartments() {
    auto unadvise = [](ITfCompartment* compartment, DWORD* cookie) {
        if (!compartment) return;
        if (*cookie != TF_INVALID_COOKIE) {
            ITfSource* source = nullptr;
            if (SUCCEEDED(compartment->QueryInterface(IID_PPV_ARGS(&source)))) {
                source->UnadviseSink(*cookie);
                source->Release();
            }
            *cookie = TF_INVALID_COOKIE;
        }
        compartment->Release();
    };
    unadvise(openCloseCompartment_, &openCloseCookie_);
    unadvise(conversionModeCompartment_, &conversionModeCookie_);
    openCloseCompartment_ = nullptr;
    conversionModeCompartment_ = nullptr;
}

void TextService::ApplyModeToCompartments() {
    updatingCompartments_ = true;
    SetCompartmentDword(openCloseCompartment_, clientId_, directMode_ ? 0 : 1);
    SetCompartmentDword(conversionModeCompartment_, clientId_,
                        directMode_ ? TF_CONVERSIONMODE_ALPHANUMERIC
                                    : kKanaConversionMode);
    updatingCompartments_ = false;
}

void TextService::SetDirectModeInternal(bool direct) {
    if (directMode_ == direct) return;
    directMode_ = direct;
    ApplyModeToCompartments();
    if (langBarButton_) langBarButton_->NotifyUpdate();
    IrohaLog(L"input mode: %s", directMode_ ? L"direct" : L"kana");
}

STDMETHODIMP TextService::OnChange(REFGUID rguid) {
    if (updatingCompartments_) return S_OK;
    // タスクバーの入力インジケーター等、外部からのモード変更に追従する
    if (IsEqualGUID(rguid, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE) ||
        IsEqualGUID(rguid, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION)) {
        const DWORD open = GetCompartmentDword(openCloseCompartment_, 1);
        const DWORD conversion =
            GetCompartmentDword(conversionModeCompartment_, kKanaConversionMode);
        const bool direct =
            open == 0 || (conversion & TF_CONVERSIONMODE_NATIVE) == 0;
        if (direct != directMode_) {
            directMode_ = direct;
            if (langBarButton_) langBarButton_->NotifyUpdate();
            IrohaLog(L"input mode (compartment): %s",
                     directMode_ ? L"direct" : L"kana");
        }
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
        if (langBarButton_) {
            ITfLangBarItemMgr* langBarMgr = nullptr;
            if (SUCCEEDED(threadMgr_->QueryInterface(IID_PPV_ARGS(&langBarMgr)))) {
                langBarMgr->RemoveItem(langBarButton_);
                langBarMgr->Release();
            }
            langBarButton_->Detach();
            langBarButton_->Release();
            langBarButton_ = nullptr;
        }
        ReleaseCompartments();
        threadMgr_->Release();
        threadMgr_ = nullptr;
    }
    // コンポジションはエディットセッションなしでは閉じられないため参照だけ手放す
    ReleaseComposition();
    ClearShown();
    DestroyPollWindow();
    if (sessionId_ != 0) {
        ConvertClient::EndSession(sessionId_);
        sessionId_ = 0;
    }
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

// キーコードを意味に読み替え、受けるかどうかを決める。
// そのキーで何をするかは変換サーバのセッションが決めるが、受けるかどうかは
// OnTestKeyDown ですぐ答える必要があるので、直近に受け取った表示（合成中か・文節変換中か）で決める:
//   文字（英字・ローマ字かなの記号）は常に、数字と Enter / Esc / Backspace / Space は合成中だけ、
//   矢印は文節変換中だけ受ける
TextService::KeyDecision TextService::DecideKey(WPARAM wParam, LPARAM lParam) const {
    KeyDecision decision;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    // 入力モードの切替キーは英数モード中でも受ける。
    //   半角/全角（VK_KANJI / VK_DBE_SBCSCHAR / VK_DBE_DBCSCHAR、US配列は Alt+`）
    //   ひらがなキー（VK_DBE_HIRAGANA/VK_DBE_KATAKANA）、無変換相当（VK_DBE_ALPHANUMERIC）
    switch (wParam) {
        case VK_KANJI: // 0x19
        case 0xF3:     // VK_DBE_SBCSCHAR（半角/全角）
        case 0xF4:     // VK_DBE_DBCSCHAR
            decision.kind = KeyDecision::Kind::ToggleDirectMode;
            return decision;
        case 0xF2: // VK_DBE_HIRAGANA
        case 0xF1: // VK_DBE_KATAKANA
            decision.kind = KeyDecision::Kind::SetKanaMode;
            return decision;
        case 0xF0: // VK_DBE_ALPHANUMERIC（英数）
            decision.kind = KeyDecision::Kind::SetDirectMode;
            return decision;
        case VK_OEM_3: // US配列の Alt+`
            if (alt) {
                decision.kind = KeyDecision::Kind::ToggleDirectMode;
                return decision;
            }
            break;
        default:
            break;
    }
    // 英数モード中はキーを一切食わない
    if (directMode_) return decision;
    // Ctrl/Altショートカットには介入しない
    if ((GetKeyState(VK_CONTROL) & 0x8000) || alt) return decision;

    const bool composing = composition_ != nullptr;
    const bool segmenting = composing && shown_.segmenting;
    decision.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const auto session = [&decision](iroha::SessionKey key) {
        decision.kind = KeyDecision::Kind::Session;
        decision.key = key;
        return decision;
    };
    switch (wParam) {
        case VK_RETURN:
            return composing ? session(iroha::SessionKey::Enter) : decision;
        case VK_ESCAPE:
            return composing ? session(iroha::SessionKey::Escape) : decision;
        case VK_BACK:
            return composing ? session(iroha::SessionKey::Backspace) : decision;
        case VK_SPACE:
            return composing ? session(iroha::SessionKey::Space) : decision;
        case VK_DOWN:
            return segmenting ? session(iroha::SessionKey::Down) : decision;
        case VK_UP:
            return segmenting ? session(iroha::SessionKey::Up) : decision;
        case VK_LEFT:
            return segmenting ? session(iroha::SessionKey::Left) : decision;
        case VK_RIGHT:
            return segmenting ? session(iroha::SessionKey::Right) : decision;
        default:
            break;
    }
    const wchar_t c = CharFromKey(wParam, lParam);
    if (c == 0) return decision;
    const bool isLetter = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
    const bool isDigit = (c >= L'0' && c <= L'9');
    if (isLetter || IsComposerSymbol(c) || (composing && isDigit)) {
        decision.character = static_cast<char32_t>(c);
        return session(iroha::SessionKey::Character);
    }
    return decision;
}

HRESULT TextService::HandleKey(ITfContext* context, const KeyDecision& decision) {
    switch (decision.kind) {
        case KeyDecision::Kind::Session:
            return SendSessionKey(context, decision.key, decision.character, decision.shift);
        case KeyDecision::Kind::ToggleDirectMode:
        case KeyDecision::Kind::SetKanaMode:
        case KeyDecision::Kind::SetDirectMode: {
            bool newDirect = directMode_;
            if (decision.kind == KeyDecision::Kind::ToggleDirectMode) newDirect = !directMode_;
            if (decision.kind == KeyDecision::Kind::SetKanaMode) newDirect = false;
            if (decision.kind == KeyDecision::Kind::SetDirectMode) newDirect = true;
            if (newDirect == directMode_) return S_OK;
            // 未確定文字列が残っていたら現状のまま確定してから切り替える
            HRESULT hr = S_OK;
            if (composition_) {
                hr = SendSessionKey(context, iroha::SessionKey::CommitNow, 0, false);
            }
            SetDirectModeInternal(newDirect);
            return hr;
        }
        case KeyDecision::Kind::None:
            break;
    }
    return S_OK;
}

HRESULT TextService::SendSessionKey(ITfContext* context, iroha::SessionKey key,
                                    char32_t character, bool shift) {
    // 合成を始めるキーには、カーソルより前の文書のテキスト（左文脈）を付ける。
    // 合成の途中では読み直さない（変換サーバは合成を始めたときの左文脈を使い続ける）
    std::u32string leftContext;
    const bool hasLeftContext = !composition_ && ReadLeftContext(context, &leftContext);

    iroha::SessionOutput output;
    if (!ConvertClient::SendKey(sessionId_, key, character, shift,
                                hasLeftContext ? &leftContext : nullptr, &output)) {
        return E_FAIL;
    }
    return ApplyOutput(context, output, /*synchronous=*/true);
}

HRESULT TextService::ApplyOutput(ITfContext* context, const iroha::SessionOutput& output,
                                 bool synchronous) {
    shown_ = output;
    shown_.commit.clear();

    HRESULT hr = S_OK;
    if (synchronous) {
        hr = RequestSyncEditSession(
            context, clientId_, [this, context, &output](TfEditCookie ec) -> HRESULT {
                return ApplyOutputInSession(ec, context, output);
            });
    } else {
        // キーの処理の外では同期のエディットセッションを取れないことがあるので、非同期で頼む。
        // 実行されるまでに新しい表示を受け取っていたら、古いほうは反映しない
        // （その間、このオブジェクトと文書を生かしておく）
        struct Keep {
            Keep(TextService* service, ITfContext* context) : service(service), context(context) {
                service->AddRef();
                context->AddRef();
            }
            ~Keep() {
                context->Release();
                service->Release();
            }
            TextService* service;
            ITfContext* context;
        };
        const auto keep = std::make_shared<Keep>(this, context);
        const HRESULT requested = RequestEditSession(
            context, clientId_,
            [this, keep, output](TfEditCookie ec) -> HRESULT {
                if (!composition_ || output.revision != shown_.revision) return S_OK;
                return ApplyOutputInSession(ec, keep->context, output);
            },
            TF_ES_ASYNCDONTCARE | TF_ES_READWRITE);
        // 反映できなくても合成は捨てない（次のキーで、変換サーバが今の表示を丸ごと返す）
        if (FAILED(requested)) IrohaLog(L"ApplyOutput(async) failed hr=0x%08X", requested);
    }
    if (FAILED(hr)) {
        // アプリが書き込みを受け付けなかった: 変換サーバの状態と画面がずれるので、合成を捨ててそろえる
        IrohaLog(L"ApplyOutput failed hr=0x%08X", hr);
        ConvertClient::ResetSession(sessionId_);
        ClearShown();
        return hr;
    }

    if (!composition_) {
        ClearShown();
        return S_OK;
    }
    if (shown_.candidatesVisible && synchronous) {
        UpdateCandidateWindow(context);
    } else {
        candidateWindow_.Hide();
    }
    if (shown_.livePending) {
        StartPolling();
    } else {
        StopPolling();
    }
    return S_OK;
}

// 確定する文字列を文書に入れ、未確定文字列を表示する（無ければ合成を終える）
HRESULT TextService::ApplyOutputInSession(TfEditCookie ec, ITfContext* context,
                                          const iroha::SessionOutput& output) {
    if (!output.commit.empty()) {
        const HRESULT hr = EnsureComposition(ec, context);
        if (FAILED(hr)) return hr;
        EndCompositionInternal(ec, context, iroha::Utf32ToUtf16(output.commit));
    }
    if (output.preedit.empty()) {
        if (composition_) EndCompositionInternal(ec, context, L"");
        return S_OK;
    }
    const HRESULT hr = EnsureComposition(ec, context);
    if (FAILED(hr)) return hr;
    return SetPreedit(ec, context, output);
}

void TextService::ClearShown() {
    shown_ = iroha::SessionOutput();
    candidateWindow_.Hide();
    StopPolling();
}

// 選んでいる文節の位置に候補ウィンドウを出す。
// 書き込んだ直後のエディットセッションでは位置が取れないことがあるので、読み取りのセッションを別に取る
HRESULT TextService::UpdateCandidateWindow(ITfContext* context) {
    if (!composition_ || !shown_.candidatesVisible || shown_.candidates.empty()) return S_OK;
    return RequestSyncEditSession(
        context, clientId_,
        [this, context](TfEditCookie ec) -> HRESULT {
            ITfContextView* view = nullptr;
            if (FAILED(context->GetActiveView(&view)) || !view) return S_OK;
            HWND owner = nullptr;
            view->GetWnd(&owner);
            if (!owner) owner = GetFocus();
            ITfRange* range = nullptr;
            if (composition_ && SUCCEEDED(composition_->GetRange(&range))) {
                // 選んでいる文節（無ければ全体）の範囲
                LONG start = 0;
                LONG end = 0;
                LONG offset = 0;
                bool found = false;
                for (const iroha::SessionOutput::Span& span : shown_.preedit) {
                    const LONG length = static_cast<LONG>(iroha::Utf32ToUtf16(span.text).size());
                    if (span.style == iroha::SessionOutput::kSelectedSegment && !found) {
                        start = offset;
                        end = offset + length;
                        found = true;
                    }
                    offset += length;
                }
                if (!found) end = offset;
                ITfRange* segmentRange = nullptr;
                if (SUCCEEDED(range->Clone(&segmentRange))) {
                    segmentRange->Collapse(ec, TF_ANCHOR_START);
                    LONG shifted = 0;
                    segmentRange->ShiftEnd(ec, end, &shifted, nullptr);
                    segmentRange->ShiftStart(ec, start, &shifted, nullptr);
                    RECT rect = {};
                    BOOL clipped = FALSE;
                    if (SUCCEEDED(view->GetTextExt(ec, segmentRange, &rect, &clipped))) {
                        candidateWindow_.Show(owner, rect, shown_.candidates,
                                              shown_.candidateAnnotations, shown_.candidateIndex);
                    }
                    segmentRange->Release();
                }
                range->Release();
            }
            view->Release();
            return S_OK;
        },
        TF_ES_SYNC | TF_ES_READ);
}

// カーソルより前の文書のテキストを読む。変換サーバが、カーソルのある行だけ・末尾40文字に整える
bool TextService::ReadLeftContext(ITfContext* context, std::u32string* text) {
    static constexpr LONG kLength = 64;
    bool succeeded = false;
    RequestSyncEditSession(
        context, clientId_,
        [context, text, &succeeded](TfEditCookie ec) -> HRESULT {
            TF_SELECTION selection = {};
            ULONG fetched = 0;
            if (FAILED(context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) ||
                fetched != 1 || !selection.range) {
                return S_OK;
            }
            ITfRange* left = nullptr;
            if (SUCCEEDED(selection.range->Clone(&left)) && left) {
                left->Collapse(ec, TF_ANCHOR_START);
                LONG shifted = 0;
                WCHAR buffer[kLength + 1];
                ULONG got = 0;
                if (SUCCEEDED(left->ShiftStart(ec, -kLength, &shifted, nullptr)) &&
                    SUCCEEDED(left->GetText(ec, 0, buffer, kLength, &got))) {
                    *text = iroha::Utf16ToUtf32(std::wstring(buffer, got));
                    succeeded = true;
                }
                left->Release();
            }
            selection.range->Release();
            return S_OK;
        },
        TF_ES_SYNC | TF_ES_READ);
    return succeeded;
}

STDMETHODIMP TextService::OnSetFocus(BOOL) {
    return S_OK;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* context, WPARAM wParam,
                                        LPARAM lParam, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = (context != nullptr &&
              DecideKey(wParam, lParam).kind != KeyDecision::Kind::None);
    return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam,
                                    BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    if (!context) return S_OK;
    const KeyDecision decision = DecideKey(wParam, lParam);
    if (decision.kind == KeyDecision::Kind::None) return S_OK;
    *eaten = TRUE;
    const HRESULT hr = HandleKey(context, decision);
    if (FAILED(hr)) {
        IrohaLog(L"HandleKey failed kind=%d key=%u hr=0x%08X", static_cast<int>(decision.kind),
                 static_cast<unsigned>(decision.key), hr);
    }
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

void TextService::OnModeButtonClicked() {
    // クリックはキーイベント外なのでコンポジションには触らず、モードだけ切り替える
    SetDirectModeInternal(!directMode_);
}

// ---- ITfCompositionSink ----

STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie,
                                                  ITfComposition* composition) {
    // アプリ側都合（フォーカス移動等）で強制終了された。
    // 表示中の文字列はそのままドキュメントに残るので、変換サーバの合成と手元の控えだけ捨てる。
    IrohaLog(L"OnCompositionTerminated");
    if (composition_ == composition && composition_) {
        ReleaseComposition();
        ConvertClient::ResetSession(sessionId_);
        ClearShown();
    }
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
    bool boldLine = false;
    if (IsEqualGUID(guid, GUID_IROHA_DISPLAY_ATTRIBUTE_CURRENT)) {
        boldLine = true;
    } else if (!IsEqualGUID(guid, GUID_IROHA_DISPLAY_ATTRIBUTE)) {
        return E_INVALIDARG;
    }
    auto* impl = new (std::nothrow) DisplayAttributeInfo(boldLine);
    if (!impl) return E_OUTOFMEMORY;
    *info = impl;
    return S_OK;
}
