#pragma once
// 専門辞書の語を、辞書ラティスで本体の辞書と同じように引くための木（読みの文字 ID でたどるトライ木）。
//
// macOS版は専門辞書を azooKey のユーザ辞書の形式（LOUDS。DictionaryBuilder.exportDictionary）に書き出し、
// azooKey がそれを本体の辞書と並べて「user」としてたどる。Windows版はファイルに書き出さず、
// 同じたどり方ができる木をメモリに作る。合わせてあること:
// - 文字 ID は本体の辞書の表（louds/charID.chid）。表に無い文字を含む読みの語は入れない
// - 1 つの読み（ノード）の語は、辞書の順・ファイルの行の順（書き出しと同じ）
// - 読みが 1 文字の語・同じ読みと語の組（2 つ目以降の辞書のもの）は入れない
// - 品詞は語の種類で決め（一般名詞・人名一般・地名一般・固有名詞）、意味は「一般」、評価値は -10

#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "iroha/term_dictionary.h"
#include "lattice/dicdata.h"

namespace iroha {

class TermLatticeIndex {
public:
    static constexpr int kRootNode = 0;

    // `charIds` は本体の辞書の文字 ID の表
    TermLatticeIndex(const std::unordered_map<char32_t, uint8_t>& charIds,
                     const std::vector<TermDictionaryPack>& packs);

    // `node` の子のうち文字 ID が `charId` のノード。無ければ nullopt
    std::optional<int> Child(int node, uint8_t charId) const;
    // `node` で終わる読みの語（無ければ空）
    const std::vector<lattice::DicdataElement>& Entries(int node) const;
    // 入れた語の数
    size_t EntryCount() const { return entryCount_; }

private:
    struct Node {
        std::vector<std::pair<uint8_t, int>> children; // 文字 ID の順
        std::vector<lattice::DicdataElement> entries;
    };
    int AddChild(int node, uint8_t charId);

    std::vector<Node> nodes_;
    size_t entryCount_ = 0;
};

} // namespace iroha
