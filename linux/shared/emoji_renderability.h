#pragma once
// この Linux が絵文字を 1 つの絵として描けるか（絵文字のフォントの字形で判定する）。
// macOS版 EmojiRenderability（Apple Color Emoji）・Windows版（Segoe UI Emoji）と同じ規則。
//
// Mozc のデータには新しい版の絵文字も入っていて、フォントが古いと □ や部品の並びになる。
// OS の版と Emoji の版の対応表は持たず、実際の字形で見る:
// すべての字形がフォントにあり（字形 0 ＝ 無し がない）、字形の数が部品（ZWJ・異体字セレクタを
// 除いたコードポイント）の数より少なければ描ける。部品が 1 つなら字形も 1 つ。字形が 1 つでも、
// 先頭の部品だけを描いたときと同じ字形なら、残りの部品は捨てられている（タグ文字の旗など）。
//
// フォントは fontconfig で「emoji」に当たるもの（Ubuntu では Noto Color Emoji）を探し、HarfBuzz でそのフォントだけで並べる。
// 候補ウィンドウを描くのは GNOME（Pango）なので、同じフォントで描かれる前提

#include <functional>
#include <memory>
#include <string>

namespace iroha {

class EmojiRenderability {
public:
    // 絵文字のフォントが見つからなければ nullptr（そのときはすべて描けるものとして扱う）
    static std::unique_ptr<EmojiRenderability> Create();
    ~EmojiRenderability();
    EmojiRenderability(const EmojiRenderability&) = delete;
    EmojiRenderability& operator=(const EmojiRenderability&) = delete;

    bool IsRenderable(const std::u32string& emoji);
    // 判定に使っているフォントのファイル
    const std::string& FontPath() const;

    // SymbolCandidateTable に渡す判定（Create できなければ空の関数）
    static std::function<bool(const std::u32string&)> MakePredicate();

private:
    struct Impl;
    explicit EmojiRenderability(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace iroha
