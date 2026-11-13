#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace iroha {

// UTF-32を正とし、境界（TSFはUTF-16、llama.cppはUTF-8）で変換する。
std::wstring Utf32ToUtf16(const std::u32string& s);
std::u32string Utf16ToUtf32(std::wstring_view s);
std::string Utf32ToUtf8(const std::u32string& s);
std::u32string Utf8ToUtf32(std::string_view s);

// 不正なバイト列があればnullopt（Utf8ToUtf32はU+FFFDに置換する寛容版）
std::optional<std::u32string> Utf8ToUtf32Strict(std::string_view s);

// 前後の空白・改行類を落とす（Swiftの.whitespacesAndNewlines相当の主要部分）
std::u32string TrimWhitespace(const std::u32string& s);

// Swiftの `Character`（書記素クラスタ）単位に分ける。
// 対象は変換エンジンが扱う文字（かな・漢字・記号・絵文字）で、結合文字・異体字セレクタ・
// ZWJでつないだ絵文字・国旗・肌の色の修飾を1文字にまとめる。Unicodeの規則の全部ではない
// （ハングルの字母の結合などは扱わない）
std::vector<std::u32string> SplitGraphemes(const std::u32string& s);
// `SplitGraphemes(s).size()` と同じ（Swiftの `String.count`）
size_t GraphemeCount(const std::u32string& s);

} // namespace iroha
