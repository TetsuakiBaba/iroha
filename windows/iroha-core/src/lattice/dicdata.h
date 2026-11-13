#pragma once
// 辞書の 1 エントリと、品詞 ID（cid）・意味 ID（mid）の定数・判定。
// 移植元: AzooKeyKanaKanjiConverter v0.11.2（MIT License, Copyright (c) 2023 Miwa / Ensan）
//   DictionaryManagement/Entry/DicdataElement.swift・CIDData.swift・MIDData.swift、
//   DictionaryManagement/DicdataStore.swift の isClause・includeMMValueCalculation
// ライセンス文は同じフォルダの LICENSE-AzooKeyKanaKanjiConverter.txt

#include <algorithm>
#include <string>
#include <utility>

namespace iroha::lattice {

// 評価値（対数確率に相当）。macOS 版の PValue（Float32）と同じ幅で、計算の順序も合わせる
using PValue = float;

namespace cid {
constexpr int kBOS = 0;
constexpr int kSymbol = 5;        // 記号
constexpr int kCommonNoun = 1285; // 一般名詞
constexpr int kProperNoun = 1288; // 固有名詞
constexpr int kNumber = 1295;     // 数
constexpr int kEOS = 1316;
constexpr int kCount = 1319;
} // namespace cid

namespace mid {
constexpr int kBOS = 500;
constexpr int kEOS = 500;
constexpr int kGeneral = 501;     // 一般
constexpr int kYear = 237;        // 年
constexpr int kNumber = 452;      // 数
constexpr int kSmallNumber = 361; // 小さい数字
constexpr int kCount = 502;
} // namespace mid

struct DicdataElement {
    std::u32string word;
    std::u32string ruby; // カタカナ
    int lcid = 0;
    int rcid = 0;
    int mid = 0;
    PValue baseValue = 0;
    PValue adjust = 0;

    DicdataElement() = default;
    DicdataElement(std::u32string word_, std::u32string ruby_, int lcid_, int rcid_, int mid_,
                   PValue value)
        : word(std::move(word_)), ruby(std::move(ruby_)), lcid(lcid_), rcid(rcid_), mid(mid_),
          baseValue(value) {}
    // 左右の品詞 ID が同じエントリ
    DicdataElement(std::u32string word_, std::u32string ruby_, int cid_, int mid_, PValue value)
        : DicdataElement(std::move(word_), std::move(ruby_), cid_, cid_, mid_, value) {}

    PValue Value() const { return std::min(PValue(0), baseValue + adjust); }
};

// 品詞 ID の種類。3 = BOS/EOS、0 = 前置の機能語、1 = 内容語、2 = 後置の機能語
int WordType(int cid);

// 左の語（右品詞 ID）と右の語（左品詞 ID）の間が文節の切れ目か
bool IsClause(int formerRcid, int latterLcid);

// 文節の意味 ID を決める語か（内容語・非自立の動詞・非自立の名詞）
bool IncludeMMValueCalculation(const DicdataElement& data);

} // namespace iroha::lattice
