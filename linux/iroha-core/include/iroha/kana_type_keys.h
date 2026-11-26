#pragma once
// 入力中の文字の種類を変える Control のキーの系列（macOS版 KanaTypeKeys.swift を移したもの。2026-11-26）。
//
// IME ごとに Control の組み合わせが違う。並びは Mozc のキー配列の表（src/data/keymap/ms-ime.tsv・kotoeri.tsv・
// atok.tsv）の ConvertTo… / DisplayAs… の行に合わせた:
//
// | 変換 | Windows（Microsoft IME） | macOS 標準（ことえり） | ATOK |
// |---|---|---|---|
// | ひらがな | Control+U | Control+J | Control+U |
// | カタカナ | Control+I | Control+K | Control+I |
// | 半角カタカナ | Control+O | Control+; | Control+O |
// | 全角英数 | Control+P | Control+L | Control+P |
// | 半角英数 | Control+T | Control+:（英語配列では Control+'） | Control+@ |
//
// F6〜F10 はどの系列でも同じなので、ここには入れない
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

enum class KanaTypeKeyStyle {
    Windows,
    Kotoeri,
    Atok,
};

// 既定は Windows（設定を足す前の iroha の動き）
constexpr KanaTypeKeyStyle kDefaultKanaTypeKeyStyle = KanaTypeKeyStyle::Windows;

// 設定の値（"windows"・"kotoeri"・"atok"。macOS版と同じキー kanaTypeKeyStyle）を読む。知らない値・空なら Windows
KanaTypeKeyStyle ParseKanaTypeKeyStyle(const std::string& value);

// 設定に書く値
const char* KanaTypeKeyStyleName(KanaTypeKeyStyle style);

// Control と一緒に押したキーの文字（英字は小文字）に当たる変換。当たらなければ nullopt
std::optional<KanaTypeConversion> KanaTypeConversionForControlKey(KanaTypeKeyStyle style, char32_t character);

} // namespace iroha
