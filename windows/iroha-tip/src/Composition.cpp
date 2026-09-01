// コンポジション（未確定文字列）の開始・更新・確定・破棄。
// すべてエディットセッション内（TfEditCookieを持つ間）でのみ文書に触れる。
#include "EditSession.h"
#include "TextService.h"

#include "iroha/unicode.h"

HRESULT TextService::EnsureComposition(TfEditCookie ec, ITfContext* context) {
    if (composition_) return S_OK;

    // 現在の選択位置に空レンジを取り、そこからコンポジションを開始する
    ITfInsertAtSelection* insert = nullptr;
    HRESULT hr = context->QueryInterface(IID_PPV_ARGS(&insert));
    if (FAILED(hr)) return hr;
    ITfRange* range = nullptr;
    hr = insert->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &range);
    insert->Release();
    if (FAILED(hr) || !range) return FAILED(hr) ? hr : E_FAIL;

    ITfContextComposition* contextComposition = nullptr;
    hr = context->QueryInterface(IID_PPV_ARGS(&contextComposition));
    if (SUCCEEDED(hr)) {
        hr = contextComposition->StartComposition(
            ec, range, static_cast<ITfCompositionSink*>(this), &composition_);
        // アプリが拒否するとS_OKでもnullが返る
        if (SUCCEEDED(hr) && !composition_) hr = E_FAIL;
        contextComposition->Release();
    }
    range->Release();
    return hr;
}

HRESULT TextService::SetCompositionText(TfEditCookie ec, ITfContext* context,
                                        const std::wstring& text, bool underline) {
    ITfRange* range = nullptr;
    HRESULT hr = composition_->GetRange(&range);
    if (FAILED(hr)) return hr;

    hr = range->SetText(ec, 0, text.c_str(), static_cast<LONG>(text.size()));
    if (SUCCEEDED(hr) && underline && displayAttributeAtom_ != TF_INVALID_GUIDATOM) {
        ITfProperty* property = nullptr;
        if (SUCCEEDED(context->GetProperty(GUID_PROP_ATTRIBUTE, &property))) {
            VARIANT var;
            VariantInit(&var);
            var.vt = VT_I4;
            var.lVal = static_cast<LONG>(displayAttributeAtom_);
            property->SetValue(ec, range, &var);
            property->Release();
        }
    }
    if (SUCCEEDED(hr)) {
        // キャレットを未確定文字列の末尾へ
        range->Collapse(ec, TF_ANCHOR_END);
        TF_SELECTION selection;
        selection.range = range;
        selection.style.ase = TF_AE_NONE;
        selection.style.fInterimChar = FALSE;
        context->SetSelection(ec, 1, &selection);
    }
    range->Release();
    return hr;
}

void TextService::ResetState() {
    composer_.Clear();
    segmented_ = false;
    segments_.clear();
    currentSegment_ = 0;
    candidateListOpen_ = false;
    conversionReading_.clear();
    conversionBaseline_.clear();
    candidateWindow_.Hide();
}

void TextService::EndCompositionInternal(TfEditCookie ec, ITfContext* context,
                                         const std::wstring& finalText) {
    SetCompositionText(ec, context, finalText, false);
    // 確定文字列に下線属性が残らないようクリアする
    ITfRange* range = nullptr;
    if (SUCCEEDED(composition_->GetRange(&range))) {
        ITfProperty* property = nullptr;
        if (SUCCEEDED(context->GetProperty(GUID_PROP_ATTRIBUTE, &property))) {
            property->Clear(ec, range);
            property->Release();
        }
        range->Release();
    }
    composition_->EndComposition(ec);
    composition_->Release();
    composition_ = nullptr;
    ResetState();
}

HRESULT TextService::ShowText(ITfContext* context, const std::wstring& text) {
    return RequestSyncEditSession(
        context, clientId_, [this, context, &text](TfEditCookie ec) -> HRESULT {
            HRESULT hr = EnsureComposition(ec, context);
            if (FAILED(hr)) {
                ResetState();
                return hr;
            }
            return SetCompositionText(ec, context, text, true);
        });
}

HRESULT TextService::UpdateComposition(ITfContext* context) {
    return ShowText(context, iroha::Utf32ToUtf16(composer_.Display()));
}

HRESULT TextService::RefreshSegmentDisplay(ITfContext* context) {
    return RequestSyncEditSession(
        context, clientId_, [this, context](TfEditCookie ec) -> HRESULT {
            HRESULT hr = EnsureComposition(ec, context);
            if (FAILED(hr)) {
                ResetState();
                return hr;
            }
            return SetSegmentedText(ec, context);
        });
}

// 文節列を未確定文字列として表示する（選択中の文節は太い下線、他は細い下線）。
// キャレットは選択中の文節の末尾に置く
HRESULT TextService::SetSegmentedText(TfEditCookie ec, ITfContext* context) {
    // 各文節のUTF-16長と全体文字列
    std::wstring text;
    std::vector<LONG> lengths;
    for (const ConversionSegment& segment : segments_) {
        const std::wstring piece = iroha::Utf32ToUtf16(segment.result);
        lengths.push_back(static_cast<LONG>(piece.size()));
        text += piece;
    }

    ITfRange* range = nullptr;
    HRESULT hr = composition_->GetRange(&range);
    if (FAILED(hr)) return hr;
    hr = range->SetText(ec, 0, text.c_str(), static_cast<LONG>(text.size()));
    if (FAILED(hr)) {
        range->Release();
        return hr;
    }

    // 文節ごとに表示属性を割り当てる
    ITfProperty* property = nullptr;
    if (SUCCEEDED(context->GetProperty(GUID_PROP_ATTRIBUTE, &property))) {
        LONG offset = 0;
        for (size_t i = 0; i < segments_.size(); ++i) {
            ITfRange* segmentRange = nullptr;
            if (SUCCEEDED(range->Clone(&segmentRange))) {
                segmentRange->Collapse(ec, TF_ANCHOR_START);
                LONG shifted = 0;
                segmentRange->ShiftEnd(ec, offset + lengths[i], &shifted, nullptr);
                segmentRange->ShiftStart(ec, offset, &shifted, nullptr);
                const TfGuidAtom atom = (i == currentSegment_)
                                            ? displayAttributeCurrentAtom_
                                            : displayAttributeAtom_;
                if (atom != TF_INVALID_GUIDATOM) {
                    VARIANT var;
                    VariantInit(&var);
                    var.vt = VT_I4;
                    var.lVal = static_cast<LONG>(atom);
                    property->SetValue(ec, segmentRange, &var);
                }
                segmentRange->Release();
            }
            offset += lengths[i];
        }
        property->Release();
    }

    // キャレットを選択中の文節の末尾へ
    LONG caret = 0;
    for (size_t i = 0; i <= currentSegment_ && i < segments_.size(); ++i) {
        caret += lengths[i];
    }
    ITfRange* caretRange = nullptr;
    if (SUCCEEDED(range->Clone(&caretRange))) {
        caretRange->Collapse(ec, TF_ANCHOR_START);
        LONG shifted = 0;
        caretRange->ShiftEnd(ec, caret, &shifted, nullptr);
        caretRange->Collapse(ec, TF_ANCHOR_END);
        TF_SELECTION selection;
        selection.range = caretRange;
        selection.style.ase = TF_AE_NONE;
        selection.style.fInterimChar = FALSE;
        context->SetSelection(ec, 1, &selection);
        caretRange->Release();
    }
    range->Release();
    return S_OK;
}

HRESULT TextService::CommitText(ITfContext* context, const std::wstring& text) {
    if (!composition_) {
        ResetState();
        return S_OK;
    }
    return RequestSyncEditSession(
        context, clientId_, [this, context, &text](TfEditCookie ec) -> HRESULT {
            EndCompositionInternal(ec, context, text);
            return S_OK;
        });
}

HRESULT TextService::CancelComposition(ITfContext* context) {
    if (!composition_) {
        ResetState();
        return S_OK;
    }
    return RequestSyncEditSession(context, clientId_,
                                  [this, context](TfEditCookie ec) -> HRESULT {
                                      EndCompositionInternal(ec, context, L"");
                                      return S_OK;
                                  });
}
