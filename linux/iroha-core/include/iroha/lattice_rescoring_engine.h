#pragma once
#include <string>
#include <vector>

#include "iroha/candidate_scorer.h"
#include "iroha/conversion_engine.h"
#include "iroha/lattice_converter.h"

namespace iroha {

// 辞書ラティスで候補を作り、モデルで並べ替える変換エンジン。
// 移植元: macos/Sources/IrohaCore/LatticeRescoringEngine.swift（挙動互換を維持すること）
//
// モデルの自由生成は読みの合わない語を出すことがあり、読み制約（ReadingConstraint）では
// 漢字・英字の読みを検証できない。候補ウィンドウ（candidateCount > 1）では候補の生成を
// 辞書ラティス（LatticeConverter）に任せ、「読みが辞書で保証された候補」とモデル自身の生成結果を
// モデルの対数確率で並べる。
// 返す候補は「モデルで並べた上位（candidateCount 以上）」＋「読みが一致する残りの辞書エントリ」で、
// candidateCount を超えることがある（候補ウィンドウはスクロールで辿る）
//
// 第一候補（candidateCount == 1、ライブ変換）は既定でモデルの生成に任せる。
// ラティスのn-bestは長い文で正解を含まないことが多く、再採点は生成より大きく劣るため
// （usesLatticeForFirstCandidate は計測・実験用）
//
// ラティスが候補を返せない読みはモデルの生成にそのまま任せる
class LatticeRescoringEngine : public ConversionEngine {
public:
    // base と scorer は同じモデルを指す（macOS版は1つの型が両方を兼ねる）。
    // どのポインタも所有しない（呼び出し側がこのエンジンより長く生かすこと）
    LatticeRescoringEngine(ConversionEngine* base, CandidateScorer* scorer,
                           LatticeConverter* lattice, int latticeCandidateCount = 10,
                           bool usesLatticeForFirstCandidate = false);

    bool Convert(const std::u32string& reading, const std::u32string& context,
                 int candidateCount, std::vector<std::u32string>* results,
                 std::string* error) override;

private:
    ConversionEngine* base_;
    CandidateScorer* scorer_;
    LatticeConverter* lattice_;
    // ラティスから取り出して採点する候補数の下限（要求候補数の方が多ければそちら）
    int latticeCandidateCount_;
    // 第一候補もラティス候補の再採点で決める（既定false。精度が落ちる）
    bool usesLatticeForFirstCandidate_;
};

} // namespace iroha
