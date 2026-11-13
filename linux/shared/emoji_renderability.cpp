#include "emoji_renderability.h"

#include <fontconfig/fontconfig.h>
#include <hb.h>

#include <cstring>
#include <vector>

namespace iroha {

namespace {

// fontconfig で「emoji」に当たるフォントのファイル（色付きの絵文字のフォントだけを受ける）
bool FindEmojiFont(std::string* path, int* index) {
    FcConfig* config = FcInitLoadConfigAndFonts();
    if (!config) return false;
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>("emoji"));
    FcPatternAddBool(pattern, FC_COLOR, FcTrue);
    FcConfigSubstitute(config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(config, pattern, &result);
    bool found = false;
    if (match) {
        FcChar8* file = nullptr;
        FcBool color = FcFalse;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch &&
            FcPatternGetBool(match, FC_COLOR, 0, &color) == FcResultMatch && color) {
            *path = reinterpret_cast<const char*>(file);
            if (FcPatternGetInteger(match, FC_INDEX, 0, index) != FcResultMatch) *index = 0;
            found = true;
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pattern);
    FcConfigDestroy(config);
    return found;
}

struct Glyphs {
    std::vector<hb_codepoint_t> ids;
    bool missing = false;
};

} // namespace

struct EmojiRenderability::Impl {
    std::string path;
    hb_blob_t* blob = nullptr;
    hb_face_t* face = nullptr;
    hb_font_t* font = nullptr;
    hb_buffer_t* buffer = nullptr;

    ~Impl() {
        if (buffer) hb_buffer_destroy(buffer);
        if (font) hb_font_destroy(font);
        if (face) hb_face_destroy(face);
        if (blob) hb_blob_destroy(blob);
    }

    Glyphs Shape(const std::u32string& text) {
        hb_buffer_clear_contents(buffer);
        hb_buffer_add_utf32(buffer, reinterpret_cast<const uint32_t*>(text.data()), static_cast<int>(text.size()), 0,
                            static_cast<int>(text.size()));
        hb_buffer_guess_segment_properties(buffer);
        // 異体字セレクタ・ZWJ など見えない文字は字形として数えない（描くときの Pango と同じ扱い）
        hb_buffer_set_flags(buffer, HB_BUFFER_FLAG_REMOVE_DEFAULT_IGNORABLES);
        hb_shape(font, buffer, nullptr, 0);
        unsigned int count = 0;
        const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &count);
        Glyphs glyphs;
        for (unsigned int i = 0; i < count; ++i) {
            glyphs.ids.push_back(info[i].codepoint);
            if (info[i].codepoint == 0) glyphs.missing = true;
        }
        return glyphs;
    }
};

EmojiRenderability::EmojiRenderability(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
EmojiRenderability::~EmojiRenderability() = default;

std::unique_ptr<EmojiRenderability> EmojiRenderability::Create() {
    auto impl = std::make_unique<Impl>();
    int index = 0;
    if (!FindEmojiFont(&impl->path, &index)) return nullptr;
    impl->blob = hb_blob_create_from_file(impl->path.c_str());
    if (!impl->blob || hb_blob_get_length(impl->blob) == 0) return nullptr;
    impl->face = hb_face_create(impl->blob, static_cast<unsigned int>(index));
    if (!impl->face || hb_face_get_glyph_count(impl->face) == 0) return nullptr;
    impl->font = hb_font_create(impl->face);
    impl->buffer = hb_buffer_create();
    if (!impl->font || !hb_buffer_allocation_successful(impl->buffer)) return nullptr;
    return std::unique_ptr<EmojiRenderability>(new EmojiRenderability(std::move(impl)));
}

const std::string& EmojiRenderability::FontPath() const { return impl_->path; }

bool EmojiRenderability::IsRenderable(const std::u32string& emoji) {
    int parts = 0;
    for (char32_t c : emoji) {
        if (c != 0x200D && !(c >= 0xFE00 && c <= 0xFE0F)) ++parts;
    }
    if (parts == 0) return false;
    const Glyphs glyphs = impl_->Shape(emoji);
    if (glyphs.missing || glyphs.ids.empty()) return false;
    const int count = static_cast<int>(glyphs.ids.size());
    if (parts == 1) return count == 1;
    if (count >= parts) return false;
    // 字形が 1 つでも、先頭の部品だけを描いたときと同じ字形なら、残りの部品は捨てられている
    if (count == 1) {
        const Glyphs base = impl_->Shape(emoji.substr(0, 1));
        if (!base.missing && base.ids.size() == 1 && base.ids[0] == glyphs.ids[0]) return false;
    }
    return true;
}

std::function<bool(const std::u32string&)> EmojiRenderability::MakePredicate() {
    std::shared_ptr<EmojiRenderability> checker = Create();
    if (!checker) return nullptr;
    return [checker](const std::u32string& emoji) { return checker->IsRenderable(emoji); };
}

} // namespace iroha
