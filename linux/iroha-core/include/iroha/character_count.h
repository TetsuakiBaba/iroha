#pragma once
#include <string>

namespace iroha {

// 選んだ文章の文字数（選択テキストの「選択した文字数の表示」）。移植元: macos/Sources/IrohaCore/CharacterCount.swift。
// 文字は書記素（Swift の Character）で数える
struct CharacterCount {
    // 改行を除いた全文字数（空白を含む）
    int total = 0;
    // 改行と空白を除いた文字数
    int withoutWhitespace = 0;
    // 空白・改行で区切ったまとまりの数（Microsoft Word と同じ数え方。「well-known」「e.g.」「2026」は各 1 語）
    int words = 0;
    // かな・漢字を含まず英字があるか（真なら単語数を出す）
    bool isEnglish = false;

    explicit CharacterCount(const std::u32string& text);

    // 表示用の短い文。日本語は「12文字」（空白があれば「12文字（空白除く 10）」）、英語は「152 words (812 chars)」
    std::u32string Summary() const;
};

} // namespace iroha
