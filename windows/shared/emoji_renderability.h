#pragma once
// この Windows が絵文字を 1 つの絵として描けるか（Segoe UI Emoji の字形で判定する）。
// macOS版 EmojiRenderability（Apple Color Emoji で判定）と同じ規則。
//
// Mozc のデータには新しい版の絵文字も入っていて、描けない Windows では □ や部品の並びになる。
// 国旗（地域指示記号の組）は Windows では絵にならず「JP」のような 2 文字になる。
// OS の版と Emoji の版の対応表は持たず、実際の字形で見る:
// すべての字形が Segoe UI Emoji にあり（字形 0 ＝ 無し がない）、字形の数が部品（ZWJ・異体字セレクタを
// 除いたコードポイント）の数より少なければ描ける。部品が 1 つなら字形も 1 つ。
//
// DirectWrite を使うので Windows 専用（iroha-server と iroha-cli に入れる。TIP には入れない）

#include <functional>
#include <memory>
#include <string>

namespace iroha {

class EmojiRenderability {
public:
    // DirectWrite を使えなければ nullptr（そのときはすべて描けるものとして扱う）
    static std::unique_ptr<EmojiRenderability> Create();
    ~EmojiRenderability();
    EmojiRenderability(const EmojiRenderability&) = delete;
    EmojiRenderability& operator=(const EmojiRenderability&) = delete;

    bool IsRenderable(const std::u32string& emoji);

    // SymbolCandidateTable に渡す判定（Create できなければ空の関数）
    static std::function<bool(const std::u32string&)> MakePredicate();

private:
    struct Impl;
    explicit EmojiRenderability(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace iroha
