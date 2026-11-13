#pragma once
// azooKey の辞書データ（azooKey_dictionary_storage の Dictionary フォルダ）を読み、
// 読みの途中から始まる語を引く。品詞の連接・意味の連接の値もここで引く。
// 移植元: AzooKeyKanaKanjiConverter v0.11.2（MIT License, Copyright (c) 2023 Miwa / Ensan）
//   Sources/KanaKanjiConverterModule/DictionaryManagement/DicdataStore.swift、
//   DictionaryManagement/DataStructure/extension LOUDS.swift
// ライセンス文は同じフォルダの LICENSE-AzooKeyKanaKanjiConverter.txt
//
// 移していないもの（iroha の呼び出し方では通らない経路）: 誤り訂正（needTypoCorrection）、
// azooKey 自身の学習（memory）、予測変換、ローマ字入力の途中の状態

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "lattice/dicdata.h"
#include "lattice/louds.h"

namespace iroha {
class TermLatticeIndex;
}

namespace iroha::lattice {

class DicdataStore {
public:
    explicit DicdataStore(std::filesystem::path dictionaryPath);

    // 辞書のエントリの最大の長さ（文字数）
    static constexpr int kMaxLength = 20;
    // この値より小さい評価値のエントリは使わない
    static constexpr PValue kThreshold = -17;

    // 引いた語と、その語が終わる位置（読みの文字位置。語の最後の文字の次）
    struct Found {
        DicdataElement data;
        int endIndex = 0;
    };

    // `characters`（カタカナにした読みを 1 文字ずつ）の `start` 文字目から始まる語を返す。
    // 並びは、この変換の間だけ加えた語 → 辞書の語（短い読みから）→ 機械的に作る語（数・1 文字のかな・記号）
    std::vector<Found> Lookup(const std::vector<std::u32string>& characters, size_t start);

    // この変換の間だけ辞書に加える語（ルビはカタカナ。読みが丸ごと一致する所にだけ入る）
    void SetDynamicUserDictionary(std::vector<DicdataElement> words);

    // 本体の辞書と一緒にたどる専門辞書（元の実装のユーザ辞書「user」にあたる。nullptr なら使わない）
    void SetTermIndex(const TermLatticeIndex* index) { termIndex_ = index; }

    // 本体の辞書の文字 ID の表（louds/charID.chid）
    const std::unordered_map<char32_t, uint8_t>& CharIds() const { return charIds_; }
    static std::unordered_map<char32_t, uint8_t> LoadCharIds(const std::filesystem::path& dictionaryPath);

    // 計算で使わないエントリか（評価値が低すぎる）
    static bool ShouldBeRemoved(const DicdataElement& data);

    // 品詞の連接の値（左の語の右品詞 ID → 右の語の左品詞 ID）。表に無ければ -25
    PValue CCValue(int former, int latter);
    // `former` の行（右の語の左品詞 ID で引く）。行が無ければ nullptr（すべて -25）
    const std::vector<PValue>* CCLine(int former);

    // 意味の連接の値
    PValue MMValue(int former, int latter) const;

private:
    uint8_t CharId(const std::u32string& character) const;
    const Louds* LoadLouds(const std::u32string& firstCharacter);
    const std::vector<uint8_t>* LoadLoudstxt(const std::string& fileId);
    // ノード番号 `nodeIndex` のエントリを `elements` に足す
    void AppendEntries(const std::string& identifier, int nodeIndex,
                       std::vector<DicdataElement>* elements);
    std::vector<DicdataElement> WiseDicdata(const std::u32string& convertTarget,
                                            size_t characterCount, size_t lowerBound,
                                            size_t upperBound,
                                            const std::vector<std::u32string>& fullText) const;

    std::filesystem::path dictionaryPath_;
    std::unordered_map<char32_t, uint8_t> charIds_;
    std::vector<PValue> mmValues_;
    // 読み込みを試したもの（無かったものは nullopt）
    std::unordered_map<std::u32string, std::optional<Louds>> loudses_;
    std::unordered_map<std::string, std::optional<std::vector<uint8_t>>> loudstxts_;
    std::vector<bool> ccParsed_;
    std::unordered_map<int, std::vector<PValue>> ccLines_;
    std::vector<DicdataElement> dynamicUserDictionary_;
    const TermLatticeIndex* termIndex_ = nullptr;
};

// ひらがな → カタカナ、カタカナ → ひらがな（かな以外はそのまま）
std::u32string ToKatakana(const std::u32string& text);
std::u32string ToHiragana(const std::u32string& text);

} // namespace iroha::lattice
