#include "iroha/engine_assembly.h"

namespace iroha {

EngineAssembly::EngineAssembly(ConversionEngine* model, CandidateScorer* scorer, LatticeConverter* lattice,
                               LatticeUse use, UserDictionaryEngine::DictionaryProvider userDictionary,
                               LearningEngine::DictionaryProvider learning,
                               TermDictionaryLiveEngine::LookupProvider liveTermDictionary) {
    ConversionEngine* core = model;
    if (use != LatticeUse::Off && lattice) {
        rescoring_ = std::make_unique<LatticeRescoringEngine>(model, scorer, lattice, 10,
                                                              use == LatticeUse::FirstCandidate);
        core = rescoring_.get();
    }
    // 異体字を候補の末尾に補い、80文字を超える読みは区切って順に変換する
    variants_ = std::make_unique<VariantKanjiEngine>(core);
    chunked_ = std::make_unique<ChunkedConversionEngine>(variants_.get());
    if (!userDictionary) userDictionary = [] { return UserDictionary(); };
    if (!learning) learning = [] { return LearningDictionary(); };
    // ライブ変換にも使う専門辞書はユーザ辞書のすぐ内側（同じ読みならユーザ辞書が勝つ）
    liveTerms_ = std::make_unique<TermDictionaryLiveEngine>(chunked_.get(), std::move(liveTermDictionary), lattice);
    dictionary_ = std::make_unique<UserDictionaryEngine>(liveTerms_.get(), std::move(userDictionary), lattice);
    learning_ = std::make_unique<LearningEngine>(dictionary_.get(), std::move(learning));
}

} // namespace iroha
