#include "TextService.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <new>

#include "ConvertClient.h"
#include "DisplayAttribute.h"
#include "EditSession.h"
#include "LangBarButton.h"
#include "key_shortcut.h"
#include "iroha/config.h"
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
    LoadVoiceKeyFromConfig();

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
    if (voiceHeld_ && sessionId_ != 0) {
        iroha::SessionOutput ignored;
        ConvertClient::SendKey(sessionId_, iroha::SessionKey::VoiceCancel, 0, false, nullptr, &ignored);
    }
    voiceHeld_ = false;
    SetVoiceContext(nullptr);
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

// 修飾キー・ロックキー（単独では何もしない。合成を確定させない）
bool IsModifierKey(WPARAM key) {
    switch (key) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN: case VK_APPS:
        case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
        case VK_PROCESSKEY: case VK_PACKET:
            return true;
        default:
            return false;
    }
}

} // namespace

// キーコードを意味に読み替え、受けるかどうかを決める。
// そのキーで何をするかは変換サーバのセッションが決めるが、受けるかどうかは
// OnTestKeyDown ですぐ答える必要があるので、直近に受け取った返事（合成中か・文節変換中か）で決める
// （macOS版 IrohaInputController.handleKey と同じ扱い）:
//   印字できる ASCII 文字は常に、Enter / Esc / Backspace / Space・矢印は合成中だけ
//   （矢印は文節変換中は文節の移動、入力中は握りつぶす）。入力していないときの Space は設定で全角にする。
//   F6〜F10・Ctrl+U/I/O/P/T は入力中（文節変換中でない）だけ、Ctrl+. は常に受ける。
//   合成中に IME が扱わないキー（Tab・Ctrl+S・Alt の組み合わせなど）は、確定してからアプリに渡す
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

    const bool composing = composition_ != nullptr;
    const bool segmenting = composing && shown_.segmenting;
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    decision.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const auto session = [&decision](iroha::SessionKey key) {
        decision.kind = KeyDecision::Kind::Session;
        decision.key = key;
        return decision;
    };
    const auto commitThenPass = [&decision, composing, wParam] {
        if (composing && !IsModifierKey(wParam)) decision.kind = KeyDecision::Kind::CommitThenPass;
        return decision;
    };
    // AI変換の結果待ち: Esc は取り消し、それ以外のキーは変換サーバに送って飲み込んでもらう
    if (composing && shown_.aiPending) {
        if (IsModifierKey(wParam)) return decision;
        if (wParam == VK_ESCAPE) return session(iroha::SessionKey::Escape);
        decision.character = 0;
        return session(iroha::SessionKey::Character);
    }
    // プライベートモードの切り替え（設定のキー。既定 Ctrl+Alt+P。入力中かどうかによらず効く。macOS版の Ctrl+Option+P）
    if (privateShortcut_ != 0) {
        const iroha::KeyShortcut shortcut = iroha::KeyShortcut::Decode(privateShortcut_);
        if (shortcut.vk == wParam && shortcut.control == control && shortcut.alt == alt && shortcut.shift == decision.shift) {
            return session(iroha::SessionKey::TogglePrivateMode);
        }
    }
    // AIで直すのキー（設定 aiFixShortcut。既定 Ctrl+Alt+A）。入力中だけ
    if (composing && !segmenting && shown_.aiFixShortcut != 0) {
        const iroha::KeyShortcut shortcut = iroha::KeyShortcut::Decode(shown_.aiFixShortcut);
        if (shortcut.vk == wParam && shortcut.control == control && shortcut.alt == alt &&
            shortcut.shift == decision.shift) {
            return session(iroha::SessionKey::AIFix);
        }
    }
    // AI変換のキー（プリセットごとに設定で決める。既定は英訳の Shift+Space）。入力中だけ。
    // Shift+Space は候補を戻す・英字の中のスペースにも使うので、Space として送り変換サーバに決めさせる
    if (composing) {
        for (size_t i = 0; i < shown_.aiShortcuts.size(); ++i) {
            if (shown_.aiShortcuts[i] == 0) continue;
            const iroha::KeyShortcut shortcut = iroha::KeyShortcut::Decode(shown_.aiShortcuts[i]);
            if (shortcut.vk == VK_SPACE && shortcut.shift && !shortcut.control && !shortcut.alt) continue;
            if (shortcut.vk == wParam && shortcut.control == control && shortcut.alt == alt &&
                shortcut.shift == decision.shift) {
                decision.character = static_cast<char32_t>(i);
                return session(iroha::SessionKey::AICommit);
            }
        }
    }
    if (alt) return commitThenPass();
    if (control) {
        if (wParam == VK_OEM_PERIOD) return session(iroha::SessionKey::TogglePunctuation);
        if (composing && !segmenting) {
            switch (wParam) {
                case 'U': return session(iroha::SessionKey::DisplayHiragana);
                case 'I': return session(iroha::SessionKey::DisplayKatakana);
                case 'O': return session(iroha::SessionKey::DisplayHalfKatakana);
                case 'P': return session(iroha::SessionKey::DisplayFullAlphanumeric);
                case 'T': return session(iroha::SessionKey::DisplayHalfAlphanumeric);
                default: break;
            }
        }
        if (wParam == VK_BACK && !composing && !idleUndoable_.empty() && !decision.shift) {
            decision.kind = KeyDecision::Kind::UndoCommit;
            return decision;
        }
        return commitThenPass();
    }
    switch (wParam) {
        case VK_F6: case VK_F7: case VK_F8: case VK_F9: case VK_F10:
            if (!composing) return decision;
            if (segmenting) return commitThenPass();
            return session(wParam == VK_F6   ? iroha::SessionKey::DisplayHiragana
                           : wParam == VK_F7 ? iroha::SessionKey::DisplayKatakana
                           : wParam == VK_F8 ? iroha::SessionKey::DisplayHalfKatakana
                           : wParam == VK_F9 ? iroha::SessionKey::DisplayFullAlphanumeric
                                             : iroha::SessionKey::DisplayHalfAlphanumeric);
        case VK_TAB:
            // 予測変換の候補が出ていれば Tab で選ぶ（Shift+Tab は前の候補）。
            // 候補ウィンドウが出ていれば、表に広げる・1 列に戻す（タブ文字はアプリに渡さない）
            if (composing && !segmenting && !shown_.predictions.empty()) return session(iroha::SessionKey::Tab);
            if (segmenting && shown_.candidatesVisible) return session(iroha::SessionKey::Tab);
            return commitThenPass();
        case VK_RETURN:
            return composing ? session(iroha::SessionKey::Enter) : decision;
        case VK_ESCAPE:
            return composing ? session(iroha::SessionKey::Escape) : decision;
        case VK_BACK:
            return composing ? session(iroha::SessionKey::Backspace) : decision;
        case VK_SPACE:
            // 入力していないときの Space は、設定で全角にしていて Shift を押していなければ受ける
            if (composing || (idleFullWidthSpace_ && !decision.shift)) return session(iroha::SessionKey::Space);
            return decision;
        case VK_DOWN:
            return composing ? session(iroha::SessionKey::Down) : decision;
        case VK_UP:
            return composing ? session(iroha::SessionKey::Up) : decision;
        case VK_LEFT:
            return composing ? session(iroha::SessionKey::Left) : decision;
        case VK_RIGHT:
            return composing ? session(iroha::SessionKey::Right) : decision;
        default:
            break;
    }
    const wchar_t c = CharFromKey(wParam, lParam);
    if (c >= 0x21 && c <= 0x7E) {
        decision.character = static_cast<char32_t>(c);
        return session(iroha::SessionKey::Character);
    }
    return commitThenPass();
}

HRESULT TextService::HandleKey(ITfContext* context, const KeyDecision& decision, bool* passToApp) {
    *passToApp = false;
    switch (decision.kind) {
        case KeyDecision::Kind::Session:
            return SendSessionKey(context, decision.key, decision.character, decision.shift);
        case KeyDecision::Kind::CommitThenPass:
            *passToApp = true;
            return composition_ ? SendSessionKey(context, iroha::SessionKey::CommitNow, 0, false) : S_OK;
        case KeyDecision::Kind::UndoCommit: {
            // カーソルの手前が確定した文字列のままのときだけ取り消す（カーソルが動いた・続けて何か入ったときは
            // 取り消さず、キーはアプリに渡す。アプリでは Ctrl+Backspace で単語を消すことが多い）
            const std::u32string text = idleUndoable_;
            idleUndoable_.clear();
            if (text.empty() || composition_ || !TextBeforeCaretIs(context, text)) {
                *passToApp = true;
                return S_OK;
            }
            replaceBeforeLength_ = static_cast<LONG>(iroha::Utf32ToUtf16(text).size());
            const HRESULT hr = SendSessionKey(context, iroha::SessionKey::UndoCommit, 0, false);
            replaceBeforeLength_ = 0;
            return hr;
        }
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
    idleUndoable_ = output.undoableCommit;
    idleFullWidthSpace_ = output.fullWidthSpace;
    voiceKey_ = output.voiceKey;
    privateShortcut_ = output.privateModeShortcut;
    if (privateMode_ != output.privateMode) {
        privateMode_ = output.privateMode;
        if (langBarButton_) langBarButton_->NotifyUpdate();
    }

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
                if (output.revision != shown_.revision) return S_OK;
                // 合成が無いところへ入れるのは、声を入れた文書だけ（ほかの文書では合成が終わったあとの古い表示）
                if (!composition_ && keep->context != voiceContext_) return S_OK;
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

    if (synchronous && !output.notice.empty()) ShowNotice(context, output.notice);
    // 合成の無い文書へ、非同期のエディットセッションで表示を入れる（声の途中結果・認識結果）
    const bool startsLater = !synchronous && !composition_ && context == voiceContext_ &&
                             (!output.preedit.empty() || !output.commit.empty());
    if (!composition_ && !startsLater) {
        if (output.voicePending || !output.voiceNotice.empty()) {
            // 合成は無いが、録音・認識の最中か、その知らせがある（結果が届くまで問い合わせる）
            candidateWindow_.Hide();
            predictionWindow_.Hide();
            UpdateTypoFeedback(context, output, synchronous);
            UpdateDebugOverlay(context, output, synchronous);
            if (output.voicePending) {
                StartPolling();
            } else {
                StopPolling();
            }
            return S_OK;
        }
        ClearShown();
        return S_OK;
    }
    if (shown_.candidatesVisible && synchronous) {
        UpdateCandidateWindow(context);
    } else {
        candidateWindow_.Hide();
    }
    if (!shown_.predictions.empty() && !shown_.segmenting && synchronous) {
        UpdatePredictionWindow(context);
    } else if (shown_.predictions.empty() || shown_.segmenting) {
        predictionWindow_.Hide();
    }
    UpdateTypoFeedback(context, output, synchronous);
    UpdateDebugOverlay(context, output, synchronous);
    // ライブ変換・打ち間違いの訂正・AI変換・AIで直す・「こえ」モードの結果待ちの間は問い合わせる
    if (shown_.livePending || shown_.typoPending || shown_.aiPending || shown_.aiFixPending || shown_.voicePending) {
        StartPolling();
    } else {
        StopPolling();
    }
    return S_OK;
}

// 確定する文字列を文書に入れ、未確定文字列を表示する（無ければ合成を終える）
HRESULT TextService::ApplyOutputInSession(TfEditCookie ec, ITfContext* context,
                                          const iroha::SessionOutput& output) {
    // 確定の取り消し: カーソルの手前の確定した文字列を覆う合成を始め、それを未確定文字列に置き換える
    if (output.replacesCommitted && !composition_ && replaceBeforeLength_ > 0 && !output.preedit.empty()) {
        const HRESULT hr = StartCompositionBeforeCaret(ec, context, replaceBeforeLength_);
        if (FAILED(hr)) return hr;
    }
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
    predictionWindow_.Hide();
    debugWindow_.Hide();
    if (!typoFeedbackShown_.empty()) {
        typoFeedbackShown_.clear();
        noticeWindow_.Hide();
    }
    StopPolling();
}

// 未確定文字列（無ければカーソル）のスクリーン座標
bool TextService::CompositionRect(ITfContext* context, RECT* rect, HWND* owner) {
    bool found = false;
    RequestSyncEditSession(
        context, clientId_,
        [this, context, rect, owner, &found](TfEditCookie ec) -> HRESULT {
            found = CompositionRectIn(ec, context, rect, owner);
            return S_OK;
        },
        TF_ES_SYNC | TF_ES_READ);
    return found;
}

bool TextService::CompositionRectIn(TfEditCookie ec, ITfContext* context, RECT* rect, HWND* owner) {
    ITfContextView* view = nullptr;
    if (FAILED(context->GetActiveView(&view)) || !view) return false;
    view->GetWnd(owner);
    if (!*owner) *owner = GetFocus();
    ITfRange* range = nullptr;
    if (composition_) {
        composition_->GetRange(&range);
    } else {
        TF_SELECTION selection = {};
        ULONG fetched = 0;
        if (SUCCEEDED(context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) && fetched == 1) {
            range = selection.range;
        }
    }
    bool found = false;
    if (range) {
        BOOL clipped = FALSE;
        found = SUCCEEDED(view->GetTextExt(ec, range, rect, &clipped));
        range->Release();
    }
    view->Release();
    return found;
}

// 打ち間違いを直したら「打った読み → 直した読み」を未確定文字列の下に出す（macOS版 showTypoFeedback）。
// ライブ変換が ON だと画面に出るのは変換後の文字列なので、読みのどこが直ったかはここでしか見せられない。
// 小窓が出ている間は Backspace で戻せる。出しっぱなしにしないよう 4 秒で閉じる（取り消しはそのまま効く）
void TextService::UpdateTypoFeedback(ITfContext* context, const iroha::SessionOutput& output, bool synchronous) {
    // 出すものの順: 打ち間違いの訂正 → AI の案（「今の変換 → AI の案」）→ AIで直すの窓・知らせ
    std::u32string text;
    if (output.typoFeedback) {
        text = output.typoFeedback->Original() + U" → " + output.typoFeedback->Corrected() + U"　（⌫ で戻す）";
    } else if (output.aiFixFeedback) {
        text = output.aiFixFeedback->Original() + U" → " + output.aiFixFeedback->Corrected() + U"　（⌫ で戻す）";
    } else if (!output.aiFixNotice.empty()) {
        text = output.aiFixNotice;
    } else {
        text = output.voiceNotice;
    }
    // 出しておく時間: 録音・認識の最中の知らせは終わるまで、音声認識との比べ（macOS版と同じ 1.5 秒・6 秒）、ほかは 4 秒
    UINT duration = 4000;
    if (!text.empty() && text == output.voiceNotice) {
        duration = output.voicePending ? 70000 : text == U"音声認識と同じ" ? 1500 : 6000;
    }
    if (text.empty()) {
        if (!typoFeedbackShown_.empty()) {
            typoFeedbackShown_.clear();
            noticeWindow_.Hide();
        }
        return;
    }
    if (text == typoFeedbackShown_ || !EnsurePollWindow()) return;
    typoFeedbackShown_ = text;
    const auto show = [this, text, duration](const RECT& rect, HWND owner) {
        noticeWindow_.Show(owner, rect, {text}, {}, SIZE_MAX, /*numbered=*/false);
        SetTimer(pollWindow_, reinterpret_cast<UINT_PTR>(this) + 1, duration, &TextService::NoticeTimerProc);
    };
    if (synchronous) {
        RECT rect = {};
        HWND owner = nullptr;
        if (CompositionRect(context, &rect, &owner)) show(rect, owner);
        return;
    }
    // 問い合わせで届いたとき: 書き込みのエディットセッションの後に、読み取りだけのセッションで位置を求める
    AddRef();
    context->AddRef();
    const HRESULT hr = RequestEditSession(
        context, clientId_,
        [this, context, show](TfEditCookie ec) -> HRESULT {
            RECT rect = {};
            HWND owner = nullptr;
            if (CompositionRectIn(ec, context, &rect, &owner)) show(rect, owner);
            context->Release();
            Release();
            return S_OK;
        },
        TF_ES_ASYNCDONTCARE | TF_ES_READ);
    if (FAILED(hr)) {
        context->Release();
        Release();
    }
}

// デバッグ表示（macOS版 DeveloperOverlay）。行は変換サーバが作る（左文脈・かな漢字変換・打ち間違いの訂正の時間）。
// 候補・予測・知らせの小窓は未確定文字列の下に出るので、こちらは上に出す。未確定文字列が伸びるので毎回置き直す
void TextService::UpdateDebugOverlay(ITfContext* context, const iroha::SessionOutput& output, bool synchronous) {
    if (output.debugOverlay.empty()) {
        debugWindow_.Hide();
        return;
    }
    std::vector<std::u32string> lines;
    size_t start = 0;
    for (;;) {
        const size_t end = output.debugOverlay.find(U'\n', start);
        lines.push_back(output.debugOverlay.substr(start, end == std::u32string::npos ? std::u32string::npos : end - start));
        if (end == std::u32string::npos) break;
        start = end + 1;
    }
    const auto show = [this, lines](const RECT& rect, HWND owner) {
        debugWindow_.Show(owner, rect, lines, {}, SIZE_MAX, /*numbered=*/false, /*expanded=*/false, /*above=*/true);
    };
    if (synchronous) {
        RECT rect = {};
        HWND owner = nullptr;
        if (CompositionRect(context, &rect, &owner)) show(rect, owner);
        return;
    }
    // 問い合わせで届いたとき（ライブ変換・休止での訂正の結果）: 読み取りだけのセッションで位置を求める
    AddRef();
    context->AddRef();
    const HRESULT hr = RequestEditSession(
        context, clientId_,
        [this, context, show](TfEditCookie ec) -> HRESULT {
            RECT rect = {};
            HWND owner = nullptr;
            if (CompositionRectIn(ec, context, &rect, &owner)) show(rect, owner);
            context->Release();
            Release();
            return S_OK;
        },
        TF_ES_ASYNCDONTCARE | TF_ES_READ);
    if (FAILED(hr)) {
        context->Release();
        Release();
    }
}

// 予測変換の候補を、未確定文字列の下に出す（番号は付けない。Tab で選ぶ）
HRESULT TextService::UpdatePredictionWindow(ITfContext* context) {
    RECT rect = {};
    HWND owner = nullptr;
    if (!CompositionRect(context, &rect, &owner)) return S_OK;
    const size_t selected = shown_.predictionIndex < 0 ? SIZE_MAX : static_cast<size_t>(shown_.predictionIndex);
    predictionWindow_.Show(owner, rect, shown_.predictions, {}, selected, /*numbered=*/false);
    return S_OK;
}

// 知らせ（句読点・プライベートモードの切り替え）をカーソルの近くに少しのあいだ出す
void TextService::ShowNotice(ITfContext* context, const std::u32string& text) {
    RECT rect = {};
    HWND owner = nullptr;
    if (!CompositionRect(context, &rect, &owner) || !EnsurePollWindow()) return;
    noticeWindow_.Show(owner, rect, {text}, {}, SIZE_MAX, /*numbered=*/false);
    SetTimer(pollWindow_, reinterpret_cast<UINT_PTR>(this) + 1, 2500, &TextService::NoticeTimerProc);
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
                        candidateWindow_.Show(owner, rect, shown_.candidates, shown_.candidateAnnotations,
                                              shown_.candidateIndex, /*numbered=*/true, shown_.candidatesExpanded);
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

// カーソルの手前（選択なし）が text のままか（確定の取り消しの前に確かめる）
bool TextService::TextBeforeCaretIs(ITfContext* context, const std::u32string& text) {
    const std::wstring expected = iroha::Utf32ToUtf16(text);
    bool matches = false;
    RequestSyncEditSession(
        context, clientId_,
        [context, &expected, &matches](TfEditCookie ec) -> HRESULT {
            TF_SELECTION selection = {};
            ULONG fetched = 0;
            if (FAILED(context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) ||
                fetched != 1 || !selection.range) {
                return S_OK;
            }
            BOOL empty = FALSE;
            ITfRange* before = nullptr;
            if (SUCCEEDED(selection.range->IsEmpty(ec, &empty)) && empty &&
                SUCCEEDED(selection.range->Clone(&before)) && before) {
                LONG shifted = 0;
                const LONG length = static_cast<LONG>(expected.size());
                std::wstring buffer(expected.size() + 1, L'\0');
                ULONG got = 0;
                if (SUCCEEDED(before->ShiftStart(ec, -length, &shifted, nullptr)) && shifted == -length &&
                    SUCCEEDED(before->GetText(ec, 0, buffer.data(), static_cast<ULONG>(buffer.size()), &got))) {
                    buffer.resize(got);
                    matches = buffer == expected;
                }
                before->Release();
            }
            selection.range->Release();
            return S_OK;
        },
        TF_ES_SYNC | TF_ES_READ);
    return matches;
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

STDMETHODIMP TextService::OnSetFocus(BOOL foreground) {
    // 設定画面で変えた「こえ」モードのキーを、変換サーバの返事が届く前から効かせる
    if (foreground) LoadVoiceKeyFromConfig();
    return S_OK;
}

// ---- 「こえ」モード ----

// 決めたキー（右 Ctrl・右 Shift）か。右 Ctrl は拡張キーの印、右 Shift はスキャンコード（0x36）で見分ける
bool TextService::IsVoiceKeyEvent(WPARAM wParam, LPARAM lParam) const {
    if (voiceKey_ == 0 || directMode_) return false;
    if (voiceKey_ == VK_RCONTROL) return (wParam == VK_CONTROL || wParam == VK_RCONTROL) && (lParam & (1 << 24)) != 0;
    if (voiceKey_ == VK_RSHIFT) return (wParam == VK_SHIFT || wParam == VK_RSHIFT) && ((lParam >> 16) & 0xFF) == 0x36;
    return false;
}

// 決めたキー以外の修飾キーが押されているか（一緒に押したときはショートカットなので録音しない）
bool TextService::OtherModifiersDown() const {
    const auto down = [](int key) { return (GetKeyState(key) & 0x8000) != 0; };
    const bool otherControl = voiceKey_ == VK_RCONTROL ? down(VK_LCONTROL) : down(VK_CONTROL);
    const bool otherShift = voiceKey_ == VK_RSHIFT ? down(VK_LSHIFT) : down(VK_SHIFT);
    return otherControl || otherShift || down(VK_MENU) || down(VK_LWIN) || down(VK_RWIN);
}

void TextService::SetVoiceContext(ITfContext* context) {
    if (context) context->AddRef();
    if (voiceContext_) voiceContext_->Release();
    voiceContext_ = context;
}

void TextService::SendVoiceKeyAsync(iroha::SessionKey key) {
    ITfContext* context = composition_ ? compositionContext_ : voiceContext_;
    if (!context || sessionId_ == 0) return;
    iroha::SessionOutput output;
    if (!ConvertClient::SendKey(sessionId_, key, 0, false, nullptr, &output)) return;
    context->AddRef();
    ApplyOutput(context, output, /*synchronous=*/false);
    context->Release();
}

void TextService::LoadVoiceKeyFromConfig() {
    wchar_t* localAppData = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&localAppData, &length, L"LOCALAPPDATA") != 0 || !localAppData) return;
    const std::wstring path = std::wstring(localAppData) + L"\\iroha\\config.json";
    free(localAppData);
    const iroha::Config config = iroha::LoadConfig(std::filesystem::path(path));
    voiceKey_ = !config.voiceInput ? 0 : config.voiceInputHoldKey == "rightShift" ? VK_RSHIFT : VK_RCONTROL;
    const auto privateShortcut = iroha::KeyShortcut::Parse(config.privateModeHotkey);
    privateShortcut_ = privateShortcut ? privateShortcut->Encode() : 0;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* context, WPARAM wParam,
                                        LPARAM lParam, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    // 「こえ」モードのキー: 押した最初の 1 回だけ受けて OnKeyDown で送る（キーはアプリにも渡す）
    if (IsVoiceKeyEvent(wParam, lParam)) {
        *eaten = context != nullptr && !voiceHeld_ && (lParam & (1 << 30)) == 0 && !OtherModifiersDown();
        return S_OK;
    }
    // 押している間にほかのキーが来た: ショートカットに使っている（録音を捨てる）
    if (voiceHeld_) {
        voiceHeld_ = false;
        SendVoiceKeyAsync(iroha::SessionKey::VoiceCancel);
    }
    *eaten = (context != nullptr &&
              DecideKey(wParam, lParam).kind != KeyDecision::Kind::None);
    // 受けないキーがアプリで何かをする（カーソルが動くなど）ので、直前の確定はもう取り消さない
    if (!*eaten && !IsModifierKey(wParam)) idleUndoable_.clear();
    return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam,
                                    BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    if (!context) return S_OK;
    if (IsVoiceKeyEvent(wParam, lParam)) {
        if (!voiceHeld_ && (lParam & (1 << 30)) == 0 && !OtherModifiersDown()) {
            voiceHeld_ = true;
            SetVoiceContext(context);
            const HRESULT hr = SendSessionKey(context, iroha::SessionKey::VoiceStart, 0, false);
            if (FAILED(hr)) voiceHeld_ = false;
        }
        return S_OK;
    }
    const KeyDecision decision = DecideKey(wParam, lParam);
    if (decision.kind == KeyDecision::Kind::None) {
        if (!IsModifierKey(wParam)) idleUndoable_.clear();
        return S_OK;
    }
    bool passToApp = false;
    const HRESULT hr = HandleKey(context, decision, &passToApp);
    // 確定してから渡すキー・取り消せなかった Ctrl+Backspace はアプリに渡す
    *eaten = passToApp ? FALSE : TRUE;
    if (FAILED(hr)) {
        IrohaLog(L"HandleKey failed kind=%d key=%u hr=0x%08X", static_cast<int>(decision.kind),
                 static_cast<unsigned>(decision.key), hr);
    }
    return S_OK;
}

// 「こえ」モードのキーを離した: OnKeyUp で送る（キーはアプリにも渡す）
STDMETHODIMP TextService::OnTestKeyUp(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = context != nullptr && voiceHeld_ && IsVoiceKeyEvent(wParam, lParam);
    return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    if (context && voiceHeld_ && IsVoiceKeyEvent(wParam, lParam)) {
        voiceHeld_ = false;
        SendSessionKey(context, iroha::SessionKey::VoiceStop, 0, false);
    }
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
