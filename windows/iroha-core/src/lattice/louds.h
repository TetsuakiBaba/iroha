#pragma once
// azooKey の辞書（LOUDS 形式のトライ木）を引く。
// 移植元: AzooKeyKanaKanjiConverter v0.11.2（MIT License, Copyright (c) 2023 Miwa / Ensan）
//   Sources/KanaKanjiConverterModule/DictionaryManagement/DataStructure/LOUDS.swift
// ライセンス文は同じフォルダの LICENSE-AzooKeyKanaKanjiConverter.txt

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace iroha::lattice {

class Louds {
public:
    // `<名前>.louds`（UInt64 の列、リトルエンディアン）と `<名前>.loudschars2`（ノードごとの文字 ID）を読む。
    // どちらかが無ければ nullopt
    static std::optional<Louds> Load(const std::filesystem::path& loudsPath,
                                     const std::filesystem::path& charsPath);

    Louds(std::vector<uint64_t> bits, const std::vector<uint8_t>& nodeIndexToCharId);

    // 根のノード番号
    static constexpr int kRootNodeIndex = 1;

    // `parentNodeIndex` の子のうち文字 ID が `charId` のノード番号。無ければ nullopt
    std::optional<int> SearchCharNodeIndex(int parentNodeIndex, uint8_t charId) const;

private:
    // `parentNodeIndex` の子ノードの番号の範囲 [first, second)
    std::pair<int, int> ChildNodeIndices(int parentNodeIndex) const;

    std::vector<uint64_t> bits_;
    // 文字 ID ごとのノード番号を、文字 ID の順に並べてつなげたもの
    std::vector<int> flatCharToNodeIndices_;
    // flatCharToNodeIndices_ のうち、文字 ID が c 以下のノードの数（c のぶんは [c-1], [c] の間）
    std::array<int, 256> flatCharToNodeIndicesIndex_{};
    // i 番目の UInt64 より左にある 0 の数
    std::vector<uint32_t> rankLarge_;
};

} // namespace iroha::lattice
