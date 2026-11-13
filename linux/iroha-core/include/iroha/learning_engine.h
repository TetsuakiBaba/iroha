#pragma once
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "iroha/conversion_engine.h"
#include "iroha/learning_dictionary.h"

namespace iroha {

// 学習結果（ユーザが修正した変換）を反映させる変換エンジンのデコレータ。
// 移植元: macos/Sources/IrohaCore/LearningEngine.swift（挙動互換を維持すること）
//
// - 入力全体の読みが過去の修正と一致し、エンジンが直す前と同じ結果を出した → その確定文字列に差し替える
//   （「直す前」の記録がない古い学習は、読みが一致すれば常に差し替える）
// - エンジンが別の結果を出した（左文脈が違うなど）→ エンジンの結果をそのまま使い、
//   候補ウィンドウでは学習結果を2番目に置く
//
// 読みが一致しなければ何もせず素通しする（文中の一部に当てはめることはしない）。
// 差し替えるかどうかをエンジンの結果で決めるので、学習済みの読みでもエンジンは毎回呼ぶ。
class LearningEngine : public ConversionEngine {
public:
    using DictionaryProvider = std::function<LearningDictionary()>;

    LearningEngine(ConversionEngine* base, DictionaryProvider dictionary);

    bool Convert(const std::u32string& reading, const std::u32string& context,
                 int candidateCount, std::vector<std::u32string>* results,
                 std::string* error) override;

    // 直近の第一候補の変換で reading に shown を返したとき、エンジン（学習を除く）が出していた結果。
    //
    // 学習の条件になる読みが無かった・別の結果を返していた場合は nullopt
    // （学習が差し替えていないので、表示した結果がそのままエンジンの結果）
    std::optional<std::u32string> EngineResult(const std::u32string& reading,
                                               const std::u32string& shown) const;

private:
    ConversionEngine* base_;
    DictionaryProvider dictionaryProvider_;
    // 読み → 直近の第一候補の変換（候補数1）で返した結果（first）とエンジンの結果（second）。
    // 修正を記録するときに「直す前にエンジンが出していた結果」を引くのに使う
    std::unordered_map<std::u32string, std::pair<std::u32string, std::u32string>> recentDecisions_;
    static constexpr size_t kMaxRecentDecisions = 256;
};

} // namespace iroha
