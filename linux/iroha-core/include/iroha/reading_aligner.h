#pragma once
#include <string>
#include <vector>

namespace iroha {

// 読み（ひらがな）と変換結果（かな漢字混じり）を突き合わせて文節に分割する。
// 移植元: macos/Sources/IrohaCore/ReadingAligner.swift（挙動互換を維持すること）
//
// 変換結果のひらがな部分（助詞・送り仮名）は読みの中にそのまま現れることを利用し、
// それらをアンカーとして漢字部分の読み区間を特定する。
// 例: 読み「きょうはいいてんきですね」 変換「今日はいい天気ですね」
//   → [(きょうはいい, 今日はいい), (てんきですね, 天気ですね)]
//
// 文節境界の初期推定用であり、正確な形態素解析ではない。
// アライメントに失敗した場合は全体を1文節として返す。
namespace ReadingAligner {

struct Segment {
    std::u32string reading;
    std::u32string conversion;

    bool operator==(const Segment& other) const {
        return reading == other.reading && conversion == other.conversion;
    }
};

std::vector<Segment> SegmentReading(const std::u32string& reading,
                                    const std::u32string& conversion);

} // namespace ReadingAligner

} // namespace iroha
