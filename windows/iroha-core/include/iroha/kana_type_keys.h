#pragma once
#include <optional>
#include <string>

namespace iroha {

// 入力中の文字の種類を変える変換（F6〜F10 と、その Control の組み合わせ）
enum class KanaTypeConversion {
    Hiragana,
    Katakana,
    HalfWidthKatakana,
    FullWidthAlphanumeric,
    HalfWidthAlphanumeric,
};

// 文字の種類を変える Control のキーの系列（設定 kanaTypeKeyStyle。2026-11-26、macOS版 KanaTypeKeys.swift を移したもの）。
//
// IME ごとに Control の組み合わせが違う。並びは Mozc のキー配列の表（src/data/keymap/ms-ime.tsv・kotoeri.tsv・
// atok.tsv）の ConvertTo… / DisplayAs… の行に合わせた:
//
//   変換         | Windows（Microsoft IME） | macOS 標準（ことえり）       | ATOK
//   ひらがな     | Ctrl+U                   | Ctrl+J                       | Ctrl+U
//   カタカナ     | Ctrl+I                   | Ctrl+K                       | Ctrl+I
//   半角カタカナ | Ctrl+O                   | Ctrl+;                       | Ctrl+O
//   全角英数     | Ctrl+P                   | Ctrl+L                       | Ctrl+P
//   半角英数     | Ctrl+T                   | Ctrl+:（英語配列では Ctrl+'） | Ctrl+@
//
// F6〜F10 はどの系列でも同じなので、ここには入れない
enum class KanaTypeKeyStyle {
    Windows,
    Kotoeri,
    Atok,
};

// 既定は Windows（設定を足す前の iroha の動き）
constexpr KanaTypeKeyStyle kDefaultKanaTypeKeyStyle = KanaTypeKeyStyle::Windows;

// 設定の値（"windows"・"kotoeri"・"atok"）を系列にする。知らない値・空なら既定（Windows）
KanaTypeKeyStyle ParseKanaTypeKeyStyle(const std::string& value);
// 系列を設定の値にする
std::string KanaTypeKeyStyleName(KanaTypeKeyStyle style);

// Control と一緒に押したキーの文字（Control を除いて求めた文字。英字は小文字）に当たる変換（macOS版の
// KanaTypeKeyStyle.conversion(forControlKey:) と同じ）
std::optional<KanaTypeConversion> KanaTypeConversionForControlKey(KanaTypeKeyStyle style, char32_t character);

// 上に Shift の扱いを足したもの。Shift つき（Ctrl+Shift+J のモード切り替えなど）は扱わない。
// @ だけは英語配列で Shift+2 なので、Shift つきでも受ける
std::optional<KanaTypeConversion> KanaTypeConversionForControlKey(KanaTypeKeyStyle style, char32_t character,
                                                                  bool shift);

} // namespace iroha
