#include "iroha/lattice_rescoring_engine.h"

#include <algorithm>
#include <cmath>

namespace iroha {

LatticeRescoringEngine::LatticeRescoringEngine(ConversionEngine* base, CandidateScorer* scorer,
                                               LatticeConverter* lattice,
                                               int latticeCandidateCount,
                                               bool usesLatticeForFirstCandidate)
    : base_(base),
      scorer_(scorer),
      lattice_(lattice),
      latticeCandidateCount_(latticeCandidateCount),
      usesLatticeForFirstCandidate_(usesLatticeForFirstCandidate) {}

bool LatticeRescoringEngine::Convert(const std::u32string& reading,
                                     const std::u32string& context, int candidateCount,
                                     std::vector<std::u32string>* results, std::string* error) {
    if (!results) return false;
    if (candidateCount <= 1 && !usesLatticeForFirstCandidate_) {
        return base_->Convert(reading, context, candidateCount, results, error);
    }
    const int scoredCount = std::max(latticeCandidateCount_, candidateCount);
    const std::vector<std::u32string> latticeCandidates =
        lattice_->FullMatchCandidates(reading, scoredCount);
    if (latticeCandidates.empty()) {
        return base_->Convert(reading, context, candidateCount, results, error);
    }
    results->clear();

    // ラティスの評価上位だけをモデルで採点して並べる。残り（単漢字・異体字・人名など、
    // 評価値は低いが読みは正しい辞書エントリ）は採点せず辞書の順で後ろに続ける
    const size_t scored = std::min<size_t>(static_cast<size_t>(scoredCount), latticeCandidates.size());
    std::vector<std::u32string> candidates(latticeCandidates.begin(),
                                           latticeCandidates.begin() + scored);
    // モデル自身の答え（ライブ変換で表示しているもの）も一緒に採点する。
    // 辞書にない語（固有名詞など）はここからしか候補に入らない
    std::vector<std::u32string> generated;
    if (!base_->Convert(reading, context, 1, &generated, error)) return false;
    if (!generated.empty() && !generated.front().empty() &&
        std::find(latticeCandidates.begin(), latticeCandidates.end(), generated.front()) ==
            latticeCandidates.end()) {
        candidates.push_back(generated.front());
    }

    std::vector<float> scores;
    if (!scorer_->Score(candidates, reading, context, &scores, error)) return false;
    if (scores.size() != candidates.size()) {
        if (error) *error = "推論に失敗: 採点結果の数が候補と合いません";
        return false;
    }
    std::vector<size_t> order;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (scores[i] > -INFINITY) order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return scores[a] > scores[b]; });
    for (const size_t index : order) results->push_back(candidates[index]);
    if (usesLatticeForFirstCandidate_ && candidateCount <= 1) {
        if (results->size() > 1) results->resize(1);
        return true;
    }
    results->insert(results->end(), latticeCandidates.begin() + scored, latticeCandidates.end());
    return true;
}

} // namespace iroha
