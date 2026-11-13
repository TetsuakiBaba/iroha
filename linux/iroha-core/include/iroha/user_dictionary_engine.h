#pragma once
#include <functional>
#include <optional>
#include <set>

#include "iroha/conversion_engine.h"
#include "iroha/lattice_converter.h"
#include "iroha/user_dictionary.h"

namespace iroha {

// ユーザ辞書を反映させる変換エンジンのデコレータ。
// 移植元: macos/Sources/IrohaCore/UserDictionaryEngine.swift（挙動互換を維持すること）
//
// LLMベースの変換エンジンには辞書を後から差し込む口がないため、読みの側で処理する:
// - 読み全体がユーザ辞書に完全一致 → その単語を最優先の候補にする
// - 読みの一部が一致 → 一致部分を単語で埋め、残りだけをエンジンに変換させて連結する
//   （例:「きららざかにいく」→「雲母坂」+ エンジンによる「にいく」→「に行く」）
//   一致を使うのは辞書ラティスで語の境目になる所だけ（lattice が無ければ読みが並べば使う）
// - 単語が読みより大幅に長いエントリ（ハッシュタグ・定型文など）は、ライブ変換（候補数1）には使わず
//   候補ウィンドウにだけ出す
//
// ユーザ辞書が空のときは何もせず素通しする（既定のふるまいと完全に同じ）。
class UserDictionaryEngine : public ConversionEngine {
public:
    using DictionaryProvider = std::function<UserDictionary()>;

    // lattice: 読みの途中の一致が語の境目に当たるかを決める辞書ラティス（無ければ nullptr）。
    // base と lattice は所有しない
    UserDictionaryEngine(ConversionEngine* base, DictionaryProvider dictionary,
                         LatticeConverter* lattice = nullptr);

    bool Convert(const std::u32string& reading, const std::u32string& context,
                 int candidateCount, std::vector<std::u32string>* results,
                 std::string* error) override;

private:
    std::optional<std::set<int>> WordBoundaries(const std::u32string& reading,
                                                const UserDictionary& dictionary,
                                                bool forLiveConversion);
    bool Compose(const std::vector<UserDictionary::Chunk>& chunks,
                 const std::u32string& context, std::u32string* out,
                 std::string* error);

    ConversionEngine* base_;
    DictionaryProvider dictionaryProvider_;
    LatticeConverter* lattice_;
};

} // namespace iroha
