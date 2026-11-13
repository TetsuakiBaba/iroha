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
    if (SUCCEEDED(hr)) {
        compositionContext_ = context;
        compositionContext_->AddRef();
    }
    return hr;
}

namespace {

// range の先頭から [start, end)（UTF-16単位）の範囲を切り出す
ITfRange* SubRange(TfEditCookie ec, ITfRange* range, LONG start, LONG end) {
    ITfRange* sub = nullptr;
    if (FAILED(range->Clone(&sub)) || !sub) return nullptr;
    sub->Collapse(ec, TF_ANCHOR_START);
    LONG shifted = 0;
    sub->ShiftEnd(ec, end, &shifted, nullptr);
    sub->ShiftStart(ec, start, &shifted, nullptr);
    return sub;
}

void SetCaret(TfEditCookie ec, ITfContext* context, ITfRange* range, LONG position) {
    ITfRange* caret = SubRange(ec, range, position, position);
    if (!caret) return;
    TF_SELECTION selection;
    selection.range = caret;
    selection.style.ase = TF_AE_NONE;
    selection.style.fInterimChar = FALSE;
    context->SetSelection(ec, 1, &selection);
    caret->Release();
}

} // namespace

// 変換サーバから受け取った未確定文字列を表示する。
// 区間ごとに下線の種類を付ける（選んでいる文節は太い下線、ほかは細い下線）。
// キャレットは、文節変換中は選んでいる文節の末尾、それ以外は全体の末尾に置く
HRESULT TextService::SetPreedit(TfEditCookie ec, ITfContext* context,
                                const iroha::SessionOutput& output) {
    if (!composition_) return E_FAIL;
    std::wstring text;
    std::vector<LONG> lengths;
    for (const iroha::SessionOutput::Span& span : output.preedit) {
        const std::wstring piece = iroha::Utf32ToUtf16(span.text);
        lengths.push_back(static_cast<LONG>(piece.size()));
        text += piece;
    }

    ITfRange* range = nullptr;
    HRESULT hr = composition_->GetRange(&range);
    if (FAILED(hr)) return hr;
    hr = range->SetText(ec, 0, text.c_str(), static_cast<LONG>(text.size()));
    // アプリがこの書き込みで合成を終わらせることがある（OnCompositionTerminated が先に呼ばれる）
    if (FAILED(hr) || !composition_) {
        range->Release();
        return FAILED(hr) ? hr : E_FAIL;
    }

    LONG caret = static_cast<LONG>(text.size());
    ITfProperty* property = nullptr;
    if (SUCCEEDED(context->GetProperty(GUID_PROP_ATTRIBUTE, &property))) {
        LONG offset = 0;
        for (size_t i = 0; i < output.preedit.size(); ++i) {
            const bool selected =
                output.preedit[i].style == iroha::SessionOutput::kSelectedSegment;
            const TfGuidAtom atom = selected ? displayAttributeCurrentAtom_ : displayAttributeAtom_;
            if (atom != TF_INVALID_GUIDATOM) {
                if (ITfRange* spanRange = SubRange(ec, range, offset, offset + lengths[i])) {
                    VARIANT var;
                    VariantInit(&var);
                    var.vt = VT_I4;
                    var.lVal = static_cast<LONG>(atom);
                    property->SetValue(ec, spanRange, &var);
                    spanRange->Release();
                }
            }
            offset += lengths[i];
            if (selected) caret = offset;
        }
        property->Release();
    } else {
        LONG offset = 0;
        for (size_t i = 0; i < output.preedit.size(); ++i) {
            offset += lengths[i];
            if (output.preedit[i].style == iroha::SessionOutput::kSelectedSegment) caret = offset;
        }
    }
    SetCaret(ec, context, range, caret);
    range->Release();
    return S_OK;
}

// 合成を finalText で終える（確定。空文字列なら取り消し）
void TextService::EndCompositionInternal(TfEditCookie ec, ITfContext* context,
                                         const std::wstring& finalText) {
    if (!composition_) return;
    ITfRange* range = nullptr;
    if (SUCCEEDED(composition_->GetRange(&range)) && range) {
        range->SetText(ec, 0, finalText.c_str(), static_cast<LONG>(finalText.size()));
        // 確定文字列に下線属性が残らないようクリアする
        ITfProperty* property = nullptr;
        if (SUCCEEDED(context->GetProperty(GUID_PROP_ATTRIBUTE, &property))) {
            property->Clear(ec, range);
            property->Release();
        }
        // キャレットを確定した文字列の末尾へ（続けて始める合成はここから始まる）
        SetCaret(ec, context, range, static_cast<LONG>(finalText.size()));
        range->Release();
    }
    // 上の書き込みでアプリが合成を終わらせていることがある
    if (composition_) composition_->EndComposition(ec);
    ReleaseComposition();
}

void TextService::ReleaseComposition() {
    if (composition_) {
        composition_->Release();
        composition_ = nullptr;
    }
    if (compositionContext_) {
        compositionContext_->Release();
        compositionContext_ = nullptr;
    }
}
