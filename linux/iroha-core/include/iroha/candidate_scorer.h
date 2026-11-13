#pragma once
#include <string>
#include <vector>

namespace iroha {

// 候補の列に、モデルから見た確からしさ（系列の対数確率）を付ける。
// 移植元: macos/Sources/IrohaCore/LatticeRescoringEngine.swift の CandidateScorer
class CandidateScorer {
public:
    virtual ~CandidateScorer() = default;
    // 成功時true。scores は candidates と同じ順の対数確率（大きいほど確からしい。
    // 採点できない候補は -inf）。失敗時falseでerrorに理由
    virtual bool Score(const std::vector<std::u32string>& candidates,
                       const std::u32string& reading, const std::u32string& context,
                       std::vector<float>* scores, std::string* error) = 0;
};

} // namespace iroha
