#pragma once
// 辞書ラティスによるかな漢字変換（読み → 候補の列）。
// 読みの各位置から始まる辞書の語を並べ、品詞の連接の値で N-best の経路を求め、
// 文全体の候補・先頭の文節だけの候補・先頭から始まる語の候補の順にまとめる。
// 移植元: AzooKeyKanaKanjiConverter v0.11.2（MIT License, Copyright (c) 2023 Miwa / Ensan）
//   Sources/KanaKanjiConverterModule/ConverterAPI/KanaKanjiConverter.swift（requestCandidates・processResult）、
//   ConversionAlgorithms/Core/FullInputProcessing.swift・Kana2Kanji.swift・RegisteredNode.swift
// ライセンス文は同じフォルダの LICENSE-AzooKeyKanaKanjiConverter.txt
//
// macOS 版の LatticeConverter が呼ぶ条件だけを移してある: 読みはかなを直接渡す（ローマ字の途中の状態なし）、
// 誤り訂正なし、予測変換なし、azooKey 自身の学習なし、Zenzai なし、読みは毎回独立（差分変換なし）

#include <string>
#include <vector>

#include "lattice/dicdata.h"
#include "lattice/dicdata_store.h"

namespace iroha::lattice {

struct ConvertedCandidate {
    std::u32string text;
    // 評価値（大きいほど良い）
    PValue value = 0;
    // 候補が覆う読みの文字数（読み全体の文字数と同じなら、読み全体を変換した候補）
    int rubyCount = 0;
    // 候補を作っている語の並び
    std::vector<DicdataElement> data;
};

class KanaKanjiConverter {
public:
    explicit KanaKanjiConverter(DicdataStore* store) : store_(store) {}

    // 元の実装の `requestCandidates(...).mainResults` と同じ並びの候補を返す。
    // `nBest` は経路の探索で各ノードに残す数（返る候補の数ではない）
    std::vector<ConvertedCandidate> RequestCandidates(const std::u32string& reading, int nBest,
                                                      bool specialCandidates);

private:
    DicdataStore* store_;
};

} // namespace iroha::lattice
