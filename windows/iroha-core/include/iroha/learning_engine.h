#pragma once
#include <functional>

#include "iroha/conversion_engine.h"
#include "iroha/learning_dictionary.h"

namespace iroha {

// 学習結果（ユーザが修正した変換）を反映させる変換エンジンのデコレータ。
// 移植元: macos/Sources/IrohaCore/LearningEngine.swift（挙動互換を維持すること）
//
// - 入力全体の読みが過去の修正と一致 → エンジンを呼ばずにその確定文字列を返す
// - 入力の一部が一致 → 左から順に、一致部分は学習結果で埋め、残りをエンジンに変換させる。
//   文節の学習は「直前までに確定した文字列」が一致するときだけ適用する
// - 候補ウィンドウでは学習結果を先頭に並べる
// 学習が空のときは何もせず素通しする。
class LearningEngine : public ConversionEngine {
public:
    using DictionaryProvider = std::function<LearningDictionary()>;

    LearningEngine(ConversionEngine* base, DictionaryProvider dictionary);

    bool Convert(const std::u32string& reading, const std::u32string& context,
                 int candidateCount, std::vector<std::u32string>* results,
                 std::string* error) override;

private:
    bool Compose(const std::u32string& reading, const std::u32string& context,
                 const LearningDictionary& dictionary, std::u32string* out,
                 std::string* error);
    bool ConvertChunk(const std::u32string& reading, const std::u32string& context,
                      std::u32string* out, std::string* error);

    ConversionEngine* base_;
    DictionaryProvider dictionaryProvider_;
};

} // namespace iroha
