#pragma once
#include <functional>
#include <memory>

#include "iroha/conversion_engine.h"
#include "iroha/lattice_converter.h"
#include "iroha/term_dictionary.h"

namespace iroha {

// 「ライブ変換にも使う」にした専門辞書の語を、ユーザ辞書と同じ規則で読みに当てる変換エンジンのデコレータ。
// 移植元: macos/Sources/IrohaCore/TermDictionary/TermDictionaryLiveEngine.swift（挙動互換を維持すること）
//
// ユーザ辞書のデコレータ（UserDictionaryEngine）のすぐ内側に置く。ユーザ辞書に一致した部分はユーザ辞書の語になり、
// 残りの読みがここに来るので、同じ読みならユーザ辞書の語が勝つ。
// 当て方は UserDictionaryEngine そのもの（読み全体の一致は置き換え、途中の一致は辞書ラティスで語の境目に
// なる所だけ。候補ウィンドウでは辞書の語を先頭に出す）。違うのは、使う語を読みごとに辞書ファイルから引くこと
// （TermDictionaryLookup::LiveDictionary。人名だけで 21 万語あり、メモリに展開しない）と、
// 同じ読みに語が 2 つ以上ある読みは使わないこと。
//
// ライブ変換に使う辞書が無ければ素通しする（既定のふるまいと同じ）
class TermDictionaryLiveEngine : public ConversionEngine {
public:
    // 変換のたびに呼ぶ。nullptr か空ならライブ変換に使う辞書は無い
    using LookupProvider = std::function<std::shared_ptr<const TermDictionaryLookup>()>;

    // base と lattice は所有しない
    TermDictionaryLiveEngine(ConversionEngine* base, LookupProvider lookup, LatticeConverter* lattice = nullptr);

    bool Convert(const std::u32string& reading, const std::u32string& context, int candidateCount,
                 std::vector<std::u32string>* results, std::string* error) override;

private:
    ConversionEngine* base_;
    LookupProvider lookup_;
    LatticeConverter* lattice_;
};

} // namespace iroha
