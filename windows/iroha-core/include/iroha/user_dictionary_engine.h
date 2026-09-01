#pragma once
#include <functional>

#include "iroha/conversion_engine.h"
#include "iroha/user_dictionary.h"

namespace iroha {

// ユーザ辞書を反映させる変換エンジンのデコレータ。
// 移植元: macos/Sources/IrohaCore/UserDictionaryEngine.swift（挙動互換を維持すること）
//
// LLMベースの変換エンジンには辞書を後から差し込む口がないため、読みの側で処理する:
// - 読み全体がユーザ辞書に完全一致 → その単語を最優先の候補にする
// - 読みの一部が一致 → 一致部分を単語で埋め、残りだけをエンジンに変換させて連結する
// ユーザ辞書が空のときは何もせず素通しする。
class UserDictionaryEngine : public ConversionEngine {
public:
    using DictionaryProvider = std::function<UserDictionary()>;

    UserDictionaryEngine(ConversionEngine* base, DictionaryProvider dictionary);

    bool Convert(const std::u32string& reading, const std::u32string& context,
                 int candidateCount, std::vector<std::u32string>* results,
                 std::string* error) override;

private:
    bool Compose(const std::vector<UserDictionary::Chunk>& chunks,
                 const std::u32string& context, std::u32string* out,
                 std::string* error);

    ConversionEngine* base_;
    DictionaryProvider dictionaryProvider_;
};

} // namespace iroha
