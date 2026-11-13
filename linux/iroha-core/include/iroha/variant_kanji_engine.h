#pragma once
#include <string>
#include <vector>

#include "iroha/conversion_engine.h"

namespace iroha {

// 人名などで使われる異体字を候補ウィンドウの末尾に補う変換エンジン。
// 移植元: macos/Sources/IrohaCore/VariantKanjiEngine.swift（挙動互換を維持すること。表も同じにする）
//
// 髙（はしごだか）・﨑（たちざき）・德・濵 のような異体字は、辞書には「髙橋」「宮﨑」の
// 複合語としてしか入っていないことが多く、「たか」「さき」と単独で打っても候補に出ない。
// モデルもこれらをほぼ生成しない。そこで読みが完全一致するときだけ、小さな内蔵表から
// 異体字を候補の末尾に足す（既にある候補は重複させない）。
// ライブ変換（candidateCount == 1）には影響しない
class VariantKanjiEngine : public ConversionEngine {
public:
    // base は所有しない（呼び出し側がこのエンジンより長く生かすこと）
    explicit VariantKanjiEngine(ConversionEngine* base) : base_(base) {}

    bool Convert(const std::u32string& reading, const std::u32string& context,
                 int candidateCount, std::vector<std::u32string>* results,
                 std::string* error) override;

    // 読み（ひらがな・カタカナ）→ 異体字。表にない読みは空。
    // 読みは UserDictionary::NormalizedReading と同じ正規化で照合する
    static std::vector<std::u32string> Variants(const std::u32string& reading);

private:
    ConversionEngine* base_;
};

} // namespace iroha
