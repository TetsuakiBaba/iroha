#include "TextService.h"

#include <new>

#include "ConvertClient.h"
#include "DisplayAttribute.h"
#include "EditSession.h"
#include "LangBarButton.h"
#include "iroha/reading_aligner.h"
#include "iroha/unicode.h"

TextService::TextService() : refCount_(1) { DllAddRef(); }

TextService::~TextService() {
    if (composition_) {
        composition_->Release();
        composition_ = nullptr;
    }
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

    // 変換サーバを先に起こしてモデルのプリロードを走らせておく
    ConvertClient::EnsureServer();
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
        threadMgr_->Release();
        threadMgr_ = nullptr;
    }
    // コンポジションはエディットセッションなしでは閉じられないため参照だけ手放す
    if (composition_) {
        composition_->Release();
        composition_ = nullptr;
    }
    ResetState();
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

TextService::KeyAction TextService::DecideKeyAction(WPARAM wParam, LPARAM lParam,
                                                    wchar_t* outChar) const {
    *outChar = 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    // 入力モードの切替キーは英数モード中でも受ける。
    //   半角/全角（VK_KANJI / VK_DBE_SBCSCHAR / VK_DBE_DBCSCHAR、US配列は Alt+`）
    //   ひらがなキー（VK_DBE_HIRAGANA/VK_DBE_KATAKANA）、無変換相当（VK_DBE_ALPHANUMERIC）
    switch (wParam) {
        case VK_KANJI: // 0x19
        case 0xF3:     // VK_DBE_SBCSCHAR（半角/全角）
        case 0xF4:     // VK_DBE_DBCSCHAR
            return KeyAction::ToggleDirectMode;
        case 0xF2: // VK_DBE_HIRAGANA
        case 0xF1: // VK_DBE_KATAKANA
            return KeyAction::SetKanaMode;
        case 0xF0: // VK_DBE_ALPHANUMERIC（英数）
            return KeyAction::SetDirectMode;
        case VK_OEM_3: // US配列の Alt+`
            if (alt) return KeyAction::ToggleDirectMode;
            break;
        default:
            break;
    }
    // 英数モード中はキーを一切食わない
    if (directMode_) return KeyAction::None;
    // Ctrl/Altショートカットには介入しない
    if ((GetKeyState(VK_CONTROL) & 0x8000) || alt) {
        return KeyAction::None;
    }
    const bool composing = composition_ != nullptr;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    switch (wParam) {
        case VK_RETURN:
            return composing ? KeyAction::Commit : KeyAction::None;
        case VK_ESCAPE:
            if (segmented_) return KeyAction::BackToComposing;
            return composing ? KeyAction::Cancel : KeyAction::None;
        case VK_BACK:
            if (segmented_) return KeyAction::BackToComposing;
            return composing ? KeyAction::Backspace : KeyAction::None;
        case VK_SPACE:
            if (segmented_) {
                return shift ? KeyAction::PrevCandidate : KeyAction::NextCandidate;
            }
            return composing ? KeyAction::Convert : KeyAction::None;
        case VK_DOWN:
            return segmented_ ? KeyAction::NextCandidate : KeyAction::None;
        case VK_UP:
            return segmented_ ? KeyAction::PrevCandidate : KeyAction::None;
        case VK_LEFT:
            if (!segmented_) return KeyAction::None;
            return shift ? KeyAction::ShrinkSegment : KeyAction::MoveSegmentLeft;
        case VK_RIGHT:
            if (!segmented_) return KeyAction::None;
            return shift ? KeyAction::ExtendSegment : KeyAction::MoveSegmentRight;
        default:
            break;
    }
    const wchar_t c = CharFromKey(wParam, lParam);
    if (c == 0) return KeyAction::None;
    if (segmented_ && candidateListOpen_ && c >= L'1' && c <= L'9') {
        *outChar = c;
        return KeyAction::SelectCandidate;
    }
    const bool isLetter = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
    const bool isDigit = (c >= L'0' && c <= L'9');
    if (isLetter || IsComposerSymbol(c) || (composing && !segmented_ && isDigit)) {
        *outChar = c;
        return segmented_ ? KeyAction::CommitThenInput : KeyAction::Input;
    }
    return KeyAction::None;
}

HRESULT TextService::HandleKey(ITfContext* context, KeyAction action,
                               wchar_t character) {
    switch (action) {
        case KeyAction::Input:
            composer_.Input(static_cast<char32_t>(character));
            return UpdateComposition(context);
        case KeyAction::Backspace:
            composer_.DeleteBackward();
            if (composer_.Empty()) return CancelComposition(context);
            return UpdateComposition(context);
        case KeyAction::Commit:
            return CommitCurrent(context);
        case KeyAction::Cancel:
            return CancelComposition(context);
        case KeyAction::Convert:
            return StartConversion(context);
        case KeyAction::NextCandidate:
            return CycleCandidate(context, +1);
        case KeyAction::PrevCandidate:
            return CycleCandidate(context, -1);
        case KeyAction::SelectCandidate:
            return SelectCandidateIndex(context,
                                        static_cast<size_t>(character - L'1'));
        case KeyAction::MoveSegmentLeft:
            return MoveSegment(context, -1);
        case KeyAction::MoveSegmentRight:
            return MoveSegment(context, +1);
        case KeyAction::ShrinkSegment:
            return ResizeSegment(context, -1);
        case KeyAction::ExtendSegment:
            return ResizeSegment(context, +1);
        case KeyAction::BackToComposing:
            segmented_ = false;
            segments_.clear();
            currentSegment_ = 0;
            candidateListOpen_ = false;
            candidateWindow_.Hide();
            return UpdateComposition(context);
        case KeyAction::CommitThenInput: {
            const HRESULT hr = CommitCurrent(context);
            if (FAILED(hr)) return hr;
            composer_.Input(static_cast<char32_t>(character));
            return UpdateComposition(context);
        }
        case KeyAction::ToggleDirectMode:
        case KeyAction::SetKanaMode:
        case KeyAction::SetDirectMode: {
            bool newDirect = directMode_;
            if (action == KeyAction::ToggleDirectMode) newDirect = !directMode_;
            if (action == KeyAction::SetKanaMode) newDirect = false;
            if (action == KeyAction::SetDirectMode) newDirect = true;
            if (newDirect == directMode_) return S_OK;
            // 未確定文字列が残っていたら現状のまま確定してから切り替える
            HRESULT hr = S_OK;
            if (composition_) hr = CommitCurrent(context);
            directMode_ = newDirect;
            if (langBarButton_) langBarButton_->NotifyUpdate();
            IrohaLog(L"input mode: %s", directMode_ ? L"direct" : L"kana");
            return hr;
        }
        case KeyAction::None:
            break;
    }
    return S_OK;
}

HRESULT TextService::StartConversion(ITfContext* context) {
    composer_.Flush();
    const std::u32string reading = composer_.Display(); // Flush後は確定かなのみ
    if (reading.empty()) return CancelComposition(context);

    // コンポジション直前の文書テキストを左文脈として渡す
    // （macOS版の「直前に確定した文字列」に相当。zenz側で末尾40文字に切り詰められる）
    const std::u32string leftContext = ReadLeftContext(context);
    std::vector<std::u32string> candidates;
    if (!ConvertClient::Convert(reading, leftContext, 9, &candidates) ||
        candidates.empty()) {
        // サーバ不調時は読みのまま表示を続ける（Enterでかな確定できる）
        IrohaLog(L"StartConversion: server unavailable");
        return UpdateComposition(context);
    }
    // 確定時の学習通知用に、読みとエンジンの第一候補を控えておく
    conversionReading_ = reading;
    conversionBaseline_ = candidates.front();
    // 第一候補を文節に分割して文節モードへ
    segments_.clear();
    for (const auto& segment :
         iroha::ReadingAligner::SegmentReading(reading, candidates.front())) {
        segments_.push_back({segment.reading, segment.conversion, {}, 0});
    }
    if (segments_.size() == 1) {
        // 全体が1文節なら取得済みの候補をそのまま流用できる
        segments_[0].candidates = std::move(candidates);
        segments_[0].candidateIndex = 0;
    }
    currentSegment_ = 0;
    segmented_ = true;
    candidateListOpen_ = false;
    candidateWindow_.Hide();
    return RefreshSegmentDisplay(context);
}

HRESULT TextService::CommitCurrent(ITfContext* context) {
    const bool wasSegmented = segmented_;
    std::u32string committed;
    if (wasSegmented) {
        committed = JoinedResults();
    } else {
        composer_.Flush();
        committed = composer_.Display();
    }
    // CommitTextで状態が消えるため先に退避する
    const std::u32string reading = conversionReading_;
    const std::u32string baseline = conversionBaseline_;
    const HRESULT hr = CommitText(context, iroha::Utf32ToUtf16(committed));
    if (SUCCEEDED(hr) && wasSegmented && !reading.empty()) {
        // エンジンの第一候補と違う確定だけがサーバ側で学習される
        ConvertClient::NotifyCommit(reading, committed, baseline);
    }
    return hr;
}

std::u32string TextService::JoinedResults() const {
    std::u32string joined;
    for (const ConversionSegment& segment : segments_) joined += segment.result;
    return joined;
}

std::u32string TextService::LeftResults(size_t segmentIndex) const {
    std::u32string left;
    for (size_t i = 0; i < segmentIndex && i < segments_.size(); ++i) {
        left += segments_[i].result;
    }
    return left;
}

HRESULT TextService::EnsureSegmentCandidates(ITfContext* context,
                                             size_t segmentIndex) {
    if (segmentIndex >= segments_.size()) return E_FAIL;
    ConversionSegment& segment = segments_[segmentIndex];
    if (!segment.candidates.empty()) return S_OK;

    // 文節の候補は「選択中の文節の読み + 左側の確定済み文字列を文脈」で生成する
    const std::u32string contextText =
        ReadLeftContext(context) + LeftResults(segmentIndex);
    std::vector<std::u32string> candidates;
    if (!ConvertClient::Convert(segment.reading, contextText, 9, &candidates) ||
        candidates.empty()) {
        IrohaLog(L"EnsureSegmentCandidates: server unavailable");
        return E_FAIL;
    }
    // 現在の結果（全体変換由来）が一覧に無ければ先頭に足す
    auto it = std::find(candidates.begin(), candidates.end(), segment.result);
    if (it == candidates.end()) {
        candidates.insert(candidates.begin(), segment.result);
        it = candidates.begin();
    }
    segment.candidateIndex = static_cast<size_t>(it - candidates.begin());
    segment.candidates = std::move(candidates);
    return S_OK;
}

HRESULT TextService::CycleCandidate(ITfContext* context, int delta) {
    if (!segmented_ || segments_.empty()) return S_OK;
    if (FAILED(EnsureSegmentCandidates(context, currentSegment_))) return S_OK;
    ConversionSegment& segment = segments_[currentSegment_];
    const size_t count = segment.candidates.size();
    segment.candidateIndex = (segment.candidateIndex + count + delta) % count;
    segment.result = segment.candidates[segment.candidateIndex];
    candidateListOpen_ = true;
    const HRESULT hr = RefreshSegmentDisplay(context);
    if (FAILED(hr)) return hr;
    return UpdateCandidateWindow(context);
}

HRESULT TextService::SelectCandidateIndex(ITfContext* context, size_t index) {
    if (!segmented_ || segments_.empty() || !candidateListOpen_) return S_OK;
    ConversionSegment& segment = segments_[currentSegment_];
    if (index >= segment.candidates.size()) return S_OK;
    segment.candidateIndex = index;
    segment.result = segment.candidates[index];
    const HRESULT hr = RefreshSegmentDisplay(context);
    if (FAILED(hr)) return hr;
    return UpdateCandidateWindow(context);
}

HRESULT TextService::MoveSegment(ITfContext* context, int delta) {
    if (!segmented_ || segments_.empty()) return S_OK;
    const size_t last = segments_.size() - 1;
    size_t next = currentSegment_;
    if (delta < 0 && next > 0) --next;
    if (delta > 0 && next < last) ++next;
    if (next == currentSegment_) return S_OK;
    currentSegment_ = next;
    candidateListOpen_ = false;
    candidateWindow_.Hide();
    return RefreshSegmentDisplay(context);
}

HRESULT TextService::ResizeSegment(ITfContext* context, int delta) {
    if (!segmented_ || segments_.empty()) return S_OK;
    // 選択文節以降の読みを結合してから境界を動かす
    std::u32string currentReading = segments_[currentSegment_].reading;
    std::u32string remainder;
    for (size_t i = currentSegment_ + 1; i < segments_.size(); ++i) {
        remainder += segments_[i].reading;
    }
    if (delta > 0) { // 伸ばす: 次の読みの先頭1文字を取り込む
        if (remainder.empty()) return S_OK;
        currentReading.push_back(remainder.front());
        remainder.erase(0, 1);
    } else { // 縮める: 末尾1文字を残りへ返す
        if (currentReading.size() <= 1) return S_OK;
        remainder.insert(remainder.begin(), currentReading.back());
        currentReading.pop_back();
    }

    const std::u32string docContext = ReadLeftContext(context);
    const std::u32string left = LeftResults(currentSegment_);
    std::vector<std::u32string> converted;
    if (!ConvertClient::Convert(currentReading, docContext + left, 1, &converted) ||
        converted.empty()) {
        return S_OK; // サーバ不調時は何もしない
    }

    std::vector<ConversionSegment> rebuilt(segments_.begin(),
                                           segments_.begin() + currentSegment_);
    rebuilt.push_back({currentReading, converted.front(), {}, 0});
    if (!remainder.empty()) {
        // 残りを変換し直して文節分割もやり直す
        std::vector<std::u32string> rest;
        if (ConvertClient::Convert(remainder, docContext + left + converted.front(), 1,
                                   &rest) &&
            !rest.empty()) {
            for (const auto& segment :
                 iroha::ReadingAligner::SegmentReading(remainder, rest.front())) {
                rebuilt.push_back({segment.reading, segment.conversion, {}, 0});
            }
        } else {
            rebuilt.push_back({remainder, remainder, {}, 0});
        }
    }
    segments_ = std::move(rebuilt);
    candidateListOpen_ = false;
    candidateWindow_.Hide();
    return RefreshSegmentDisplay(context);
}

HRESULT TextService::UpdateCandidateWindow(ITfContext* context) {
    if (!composition_ || !segmented_ || segments_.empty()) return S_OK;
    const ConversionSegment& segment = segments_[currentSegment_];
    if (segment.candidates.empty()) return S_OK;
    return RequestSyncEditSession(
        context, clientId_,
        [this, context, &segment](TfEditCookie ec) -> HRESULT {
            ITfContextView* view = nullptr;
            if (FAILED(context->GetActiveView(&view)) || !view) return S_OK;
            HWND owner = nullptr;
            view->GetWnd(&owner);
            if (!owner) owner = GetFocus();
            // 選択中の文節のレンジを切り出してその位置に出す
            ITfRange* range = nullptr;
            if (composition_ && SUCCEEDED(composition_->GetRange(&range))) {
                LONG start = 0;
                LONG end = 0;
                for (size_t i = 0; i < segments_.size(); ++i) {
                    const LONG length = static_cast<LONG>(
                        iroha::Utf32ToUtf16(segments_[i].result).size());
                    if (i < currentSegment_) start += length;
                    if (i <= currentSegment_) end += length;
                }
                ITfRange* segmentRange = nullptr;
                if (SUCCEEDED(range->Clone(&segmentRange))) {
                    segmentRange->Collapse(ec, TF_ANCHOR_START);
                    LONG shifted = 0;
                    segmentRange->ShiftEnd(ec, end, &shifted, nullptr);
                    segmentRange->ShiftStart(ec, start, &shifted, nullptr);
                    RECT rect = {};
                    BOOL clipped = FALSE;
                    if (SUCCEEDED(view->GetTextExt(ec, segmentRange, &rect, &clipped))) {
                        candidateWindow_.Show(owner, rect, segment.candidates,
                                              segment.candidateIndex);
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

std::u32string TextService::ReadLeftContext(ITfContext* context) {
    std::u32string leftContext;
    if (!composition_) return leftContext;
    RequestSyncEditSession(
        context, clientId_,
        [this, &leftContext](TfEditCookie ec) -> HRESULT {
            ITfRange* range = nullptr;
            if (!composition_ || FAILED(composition_->GetRange(&range))) return S_OK;
            ITfRange* left = nullptr;
            if (SUCCEEDED(range->Clone(&left))) {
                left->Collapse(ec, TF_ANCHOR_START);
                LONG shifted = 0;
                left->ShiftStart(ec, -40, &shifted, nullptr);
                WCHAR buffer[64];
                ULONG got = 0;
                if (SUCCEEDED(left->GetText(ec, 0, buffer, ARRAYSIZE(buffer), &got))) {
                    leftContext = iroha::Utf16ToUtf32(std::wstring(buffer, got));
                }
                left->Release();
            }
            range->Release();
            return S_OK;
        },
        TF_ES_SYNC | TF_ES_READ);
    return leftContext;
}

STDMETHODIMP TextService::OnSetFocus(BOOL) {
    return S_OK;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* context, WPARAM wParam,
                                        LPARAM lParam, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    wchar_t c = 0;
    *eaten = (context != nullptr &&
              DecideKeyAction(wParam, lParam, &c) != KeyAction::None);
    return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam,
                                    BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    if (!context) return S_OK;
    wchar_t c = 0;
    const KeyAction action = DecideKeyAction(wParam, lParam, &c);
    if (action == KeyAction::None) return S_OK;
    *eaten = TRUE;
    const HRESULT hr = HandleKey(context, action, c);
    if (FAILED(hr)) IrohaLog(L"HandleKey failed action=%d hr=0x%08X", action, hr);
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
    directMode_ = !directMode_;
    if (langBarButton_) langBarButton_->NotifyUpdate();
    IrohaLog(L"input mode (click): %s", directMode_ ? L"direct" : L"kana");
}

// ---- ITfCompositionSink ----

STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie,
                                                  ITfComposition* composition) {
    // アプリ側都合（フォーカス移動等）で強制終了された。
    // 表示中の文字列はそのままドキュメントに残るので、内部状態だけ捨てる。
    IrohaLog(L"OnCompositionTerminated");
    if (composition_ == composition && composition_) {
        composition_->Release();
        composition_ = nullptr;
    }
    ResetState();
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
