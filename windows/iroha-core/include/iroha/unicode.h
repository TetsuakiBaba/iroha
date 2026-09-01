#pragma once
#include <string>
#include <string_view>

namespace iroha {

// UTF-32を正とし、境界（TSFはUTF-16、llama.cppはUTF-8）で変換する。
std::wstring Utf32ToUtf16(const std::u32string& s);
std::u32string Utf16ToUtf32(std::wstring_view s);
std::string Utf32ToUtf8(const std::u32string& s);
std::u32string Utf8ToUtf32(std::string_view s);

} // namespace iroha
