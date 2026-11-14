#pragma once
// かな漢字変換エンジンの組み立て（デコレータの重ね方）。変換サーバ（iroha-server）・iroha-cli・iroha-bench が同じものを使う。
// 移植元: macos/Sources/IrohaEngine/EngineAssembly.swift
//
// 外側から 学習 → ユーザ辞書 → 長い読みの区切り → 異体字の補完 → 辞書ラティス + モデルの採点 → モデル の順に重ねる
// （macOS版の「自分の原稿の表記」は Windows版に無い）。学習・ユーザ辞書は変換のたびに関数から読むので、
// 空を渡せば素通しになる（評価ではモデル単体の力を測るため空にする）。
// **デコレータを足す・並べ替えるときはここを変える**（変換サーバだけに足すと、ベンチマークの数字が実際の入力と合わなくなる）

#include <memory>

#include "iroha/candidate_scorer.h"
#include "iroha/chunked_conversion_engine.h"
#include "iroha/conversion_engine.h"
#include "iroha/lattice_converter.h"
#include "iroha/lattice_rescoring_engine.h"
#include "iroha/learning_engine.h"
#include "iroha/user_dictionary_engine.h"
#include "iroha/variant_kanji_engine.h"

namespace iroha {

// 辞書ラティスの使い方
enum class LatticeUse {
    // 使わない（モデルだけ。`IROHA_LATTICE=off`）
    Off,
    // 候補ウィンドウの候補にだけ使い、第一候補はモデルの生成のまま（IME の既定）
    Candidates,
    // 第一候補も辞書ラティスの候補をモデルで採点し直して選ぶ（`IROHA_LATTICE=always`）
    FirstCandidate,
};

class EngineAssembly {
public:
    // model と scorer は同じモデル（ZenzEngine）を指す。lattice は無ければ nullptr（そのときはモデルだけ）。
    // どのポインタも所有しない（呼び出し側がこの組み立てより長く生かすこと）。
    // userDictionary・learning が空の関数なら、空の辞書を使う
    EngineAssembly(ConversionEngine* model, CandidateScorer* scorer, LatticeConverter* lattice, LatticeUse use,
                   UserDictionaryEngine::DictionaryProvider userDictionary = {},
                   LearningEngine::DictionaryProvider learning = {});
    EngineAssembly(const EngineAssembly&) = delete;
    EngineAssembly& operator=(const EngineAssembly&) = delete;

    // 一番外側（学習）。変換サーバは EngineResult で学習を除いた結果を引く
    LearningEngine& Engine() { return *learning_; }

private:
    std::unique_ptr<LatticeRescoringEngine> rescoring_;
    std::unique_ptr<VariantKanjiEngine> variants_;
    std::unique_ptr<ChunkedConversionEngine> chunked_;
    std::unique_ptr<UserDictionaryEngine> dictionary_;
    std::unique_ptr<LearningEngine> learning_;
};

} // namespace iroha
