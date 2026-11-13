#include "emoji_renderability.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwrite.h>

#include <wrl/client.h>

#include "iroha/unicode.h"

namespace iroha {

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kEmojiFamily[] = L"Segoe UI Emoji";

// レイアウトを「描く」ふりをして、字形の数・字形 0 の有無・使われたフォントを数える
class GlyphCounter : public IDWriteTextRenderer {
public:
    explicit GlyphCounter(IDWriteFontCollection* collection) : collection_(collection) {}

    int glyphCount = 0;
    bool usable = true;
    // 描いた字形（字形が 1 つのときに、先頭の部品だけの字形と比べるのに使う）
    UINT16 firstGlyph = 0;

    // IUnknown（スタックに置く使い捨てなので参照数は数えない）
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) ||
            riid == __uuidof(IDWriteTextRenderer)) {
            *object = static_cast<IDWriteTextRenderer*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    // IDWritePixelSnapping
    STDMETHODIMP IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        *disabled = FALSE;
        return S_OK;
    }
    STDMETHODIMP GetCurrentTransform(void*, DWRITE_MATRIX* transform) override {
        *transform = DWRITE_MATRIX{1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    STDMETHODIMP GetPixelsPerDip(void*, FLOAT* pixelsPerDip) override {
        *pixelsPerDip = 1;
        return S_OK;
    }

    // IDWriteTextRenderer
    STDMETHODIMP DrawGlyphRun(void*, FLOAT, FLOAT, DWRITE_MEASURING_MODE,
                              const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*,
                              IUnknown*) override {
        if (glyphCount == 0 && run->glyphCount > 0) firstGlyph = run->glyphIndices[0];
        glyphCount += static_cast<int>(run->glyphCount);
        for (UINT32 i = 0; i < run->glyphCount; ++i) {
            if (run->glyphIndices[i] == 0) usable = false;
        }
        if (!IsEmojiFace(run->fontFace)) usable = false;
        return S_OK;
    }
    STDMETHODIMP DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override {
        return S_OK;
    }
    STDMETHODIMP DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*,
                                   IUnknown*) override {
        return S_OK;
    }
    STDMETHODIMP DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL,
                                  IUnknown*) override {
        return S_OK;
    }

private:
    // Segoe UI Emoji で描かれたか（ほかのフォントに落ちていれば、そのフォントには絵がない）
    bool IsEmojiFace(IDWriteFontFace* face) const {
        if (!face || !collection_) return false;
        ComPtr<IDWriteFont> font;
        ComPtr<IDWriteFontFamily> family;
        ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(collection_->GetFontFromFontFace(face, &font)) ||
            FAILED(font->GetFontFamily(&family)) || FAILED(family->GetFamilyNames(&names))) {
            return false;
        }
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists) index = 0;
        UINT32 length = 0;
        if (FAILED(names->GetStringLength(index, &length))) return false;
        std::wstring name(length + 1, L'\0');
        if (FAILED(names->GetString(index, name.data(), length + 1))) return false;
        name.resize(length);
        return name == kEmojiFamily;
    }

    IDWriteFontCollection* collection_;
};

} // namespace

struct EmojiRenderability::Impl {
    ComPtr<IDWriteFactory> factory;
    ComPtr<IDWriteFontCollection> collection;
    ComPtr<IDWriteTextFormat> format;
};

EmojiRenderability::EmojiRenderability(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
EmojiRenderability::~EmojiRenderability() = default;

std::unique_ptr<EmojiRenderability> EmojiRenderability::Create() {
    auto impl = std::make_unique<Impl>();
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(impl->factory.GetAddressOf()))) ||
        FAILED(impl->factory->GetSystemFontCollection(&impl->collection)) ||
        FAILED(impl->factory->CreateTextFormat(kEmojiFamily, impl->collection.Get(),
                                               DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"ja-jp",
                                               &impl->format))) {
        return nullptr;
    }
    // Segoe UI Emoji が入っていなければ判定できない
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(impl->collection->FindFamilyName(kEmojiFamily, &index, &exists)) || !exists) {
        return nullptr;
    }
    return std::unique_ptr<EmojiRenderability>(new EmojiRenderability(std::move(impl)));
}

namespace {

// text を Segoe UI Emoji で配置して字形を数える。配置できなければ偽
bool CountGlyphs(IDWriteFactory* factory, IDWriteTextFormat* format, const std::u32string& emoji,
                 GlyphCounter* counter) {
    const std::wstring text = Utf32ToUtf16(emoji);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                         1000.0f, 100.0f, &layout))) {
        return false;
    }
    return SUCCEEDED(layout->Draw(nullptr, counter, 0, 0));
}

} // namespace

bool EmojiRenderability::IsRenderable(const std::u32string& emoji) {
    int parts = 0;
    for (char32_t c : emoji) {
        if (c != 0x200D && !(c >= 0xFE00 && c <= 0xFE0F)) ++parts;
    }
    if (parts == 0) return false;
    GlyphCounter counter(impl_->collection.Get());
    if (!CountGlyphs(impl_->factory.Get(), impl_->format.Get(), emoji, &counter) || !counter.usable) {
        return false;
    }
    if (parts == 1) return counter.glyphCount == 1;
    if (counter.glyphCount < 1 || counter.glyphCount >= parts) return false;
    // 字形が 1 つでも、先頭の部品だけを描いたときと同じ字形なら、残りの部品は捨てられている
    // （イングランドの旗 🏴 + タグ文字は、Windows ではタグ文字を捨てて黒い旗だけになる）
    if (counter.glyphCount == 1) {
        GlyphCounter base(impl_->collection.Get());
        if (CountGlyphs(impl_->factory.Get(), impl_->format.Get(), emoji.substr(0, 1), &base) &&
            base.glyphCount == 1 && base.firstGlyph == counter.firstGlyph) {
            return false;
        }
    }
    return true;
}

std::function<bool(const std::u32string&)> EmojiRenderability::MakePredicate() {
    std::shared_ptr<EmojiRenderability> checker = Create();
    if (!checker) return nullptr;
    return [checker](const std::u32string& emoji) { return checker->IsRenderable(emoji); };
}

} // namespace iroha
