#pragma once
#include <string>

namespace iroha {

// ひらがな→カタカナ変換（zenzのプロンプトはカタカナ読みを要求する）。
// ぁ(U+3041)〜ゖ(U+3096) → ァ(U+30A1)〜ヶ(U+30F6)。それ以外は素通し。
// 移植元: macos/Sources/IrohaCore/ConversionEngine.swift hiraganaToKatakana
std::u32string HiraganaToKatakana(const std::u32string& s);

} // namespace iroha
