#pragma once
#include <string>

namespace iroha {

// ひらがな→カタカナ変換（zenzのプロンプトはカタカナ読みを要求する）。
// ぁ(U+3041)〜ゖ(U+3096) → ァ(U+30A1)〜ヶ(U+30F6)。それ以外は素通し。
// 移植元: macos/Sources/IrohaCore/ConversionEngine.swift hiraganaToKatakana
std::u32string HiraganaToKatakana(const std::u32string& s);

// カタカナ→ひらがな変換（HiraganaToKatakanaの逆）。
// ァ(U+30A1)〜ヶ(U+30F6) → ぁ(U+3041)〜ゖ(U+3096)。それ以外は素通し。
// 移植元: macos/Sources/IrohaCore/ReadingAligner.swift katakanaToHiragana
std::u32string KatakanaToHiragana(const std::u32string& s);

// 全角 → 半角（F8 の半角カタカナ用。macOS版の applyingTransform(.fullwidthToHalfwidth) にあたる）。
// カタカナは半角カタカナ（濁点・半濁点は分ける。ガ → ｶﾞ）、全角英数記号は ASCII、全角スペースは半角に。
// 半角の無い文字（ヮ・ヵ・ひらがな・漢字など）はそのまま
std::u32string ToHalfwidth(const std::u32string& s);

// ASCII → 全角（F9 の全角英数用）。! 〜 ~ は U+FF01〜U+FF5E、スペースは全角スペース。それ以外はそのまま
std::u32string ToFullwidth(const std::u32string& s);

} // namespace iroha
