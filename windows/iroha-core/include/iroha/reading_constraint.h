#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace iroha {

// 生成中の出力が入力読みと辻褄が合っているかを追跡する制約（constrained decoding）。
// 移植元: macos/Sources/IrohaCore/ReadingConstraint.swift（挙動互換を維持すること）
//
// zenzは自由生成なので、読みに存在しない「。」や「、」を勝手に挿入したり、
// 読みを食い残したまま終わったりする（例: こんにちはあかちゃん → こんにちは。赤ちゃん）。
// 「出力の各文字が読みの何文字を消費したか」だけを追う軽量な制約で防ぐ。
//
// - ひらがな・句読点・記号は読みにそのまま現れるはずなので、その位置と一致しなければ不許可
// - 漢字・カタカナ・英数（読みが不定の文字）は読みを1〜maxSpan文字消費したものとみなす
// - 終端は読みを使い切ったときのみ許可する
//
// 消費位置は「ありうる位置の集合」をビットマスクで持つ（bit p = 読みをp文字消費した状態）。
//
// 注意: Swift版はCharacter（グラフェムクラスタ）単位、この移植はコードポイント単位。
// 読みもトークンも精成済み（NFC）かなである限り両者は一致する。結合濁点などを含む
// 入力はサーバ境界でNFC正規化してから渡すこと。
class ReadingConstraint {
public:
    // ビットマスクに収まる読みの最大長。これを超える読みでは制約をかけない
    static constexpr int kMaxReadingLength = 62;

    // 読みが空、または長すぎて追跡できない場合はnullopt（制約なしで生成する）
    static std::optional<ReadingConstraint> Create(const std::u32string& reading);

    // 何も消費していない初期状態
    uint64_t InitialMask() const { return 1; }

    // 読みを使い切った状態を含むか（終端を許可してよいか）
    bool IsComplete(uint64_t mask) const;

    // 出力文字列を1つ消費した後の状態。0なら制約違反（ありうる位置がない）
    uint64_t Advance(uint64_t mask, const std::u32string& text) const;
    uint64_t Advance(uint64_t mask, char32_t character) const;

    // 1文字が読みを何文字消費しうるか
    enum class Span {
        Literal,    // 読みにそのまま現れるはず（ひらがな・句読点・記号）
        OneOrMore,  // 読みは不定だが必ず1文字以上消費する（漢字・々）
        ZeroOrMore, // 読みを消費しないこともある（カタカナ・英数字）
    };
    static Span SpanOf(char32_t character);

private:
    // 漢字1文字が持ちうる読みの最大長（承る=うけたまわ など）
    static constexpr int kMaxSpan = 8;

    ReadingConstraint(int readingLength,
                      std::unordered_map<char32_t, uint64_t> literalPositions);

    int readingLength_;
    // 文字 → その文字が読みのどの位置に現れるか（bit p = reading[p]がその文字）
    std::unordered_map<char32_t, uint64_t> literalPositions_;
};

} // namespace iroha
