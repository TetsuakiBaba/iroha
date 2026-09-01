#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace iroha {

// ユーザが変換を修正したときに記録する1エントリ。
// 移植元: macos/Sources/IrohaCore/LearningDictionary.swift（JSON形式の互換を維持）
struct LearningEntry {
    enum class Kind {
        Sentence, // 入力全体（読み全体 → 確定文字列）
        Segment,  // 文節（読み → 変換結果、直前の文脈つき）
    };

    Kind kind = Kind::Sentence;
    std::u32string reading;     // ひらがなの読み
    std::u32string result;      // 確定された変換結果
    std::u32string leftContext; // 直前までに確定していた文字列の末尾（sentenceでは未使用）
    int64_t updatedAt = 0;      // Unix時間（秒）。JSONではISO8601文字列
};

// 変換時に参照する学習結果の不変スナップショット。
//
// 同じ読みでも文中の位置によって正解が違う（「きしゃのきしゃ」→「記者の貴社」）ため、
// 文節の学習は「直前までに確定した文字列」を条件に付けて記録・適用する。
class LearningDictionary {
public:
    // 文脈として覚える文字数
    static constexpr int kContextLength = 6;
    // 文中の一部として当てはめる最短の読みの長さ（1文字だと無差別に一致して変換が壊れる）
    static constexpr int kMinimumMatchLength = 2;

    LearningDictionary() = default; // empty
    explicit LearningDictionary(std::vector<LearningEntry> entries);

    bool IsEmpty() const { return sentences_.empty() && segments_.empty(); }
    const std::vector<LearningEntry>& Entries() const { return entries_; }

    // 入力全体が過去に確定した読みと完全一致するならその確定文字列（無ければnullptr）
    const std::u32string* Sentence(const std::u32string& reading) const;

    // 位置indexから始まる学習済み文節が適用され得るか（文脈条件込みの安価な事前判定）。
    // 「変換してみるまで分からない」条件（空でないleftContext）はここでは真として扱う
    bool MayMatch(const std::u32string& characters, size_t index, bool atStart) const;

    // 位置indexから始まる学習済み文節のうち、文脈条件を満たす最長のもの（無ければnullptr）
    const LearningEntry* BestMatch(const std::u32string& characters, size_t index,
                                   const std::u32string& leftContext,
                                   bool atStart) const;

    // 文脈条件の判定
    bool Applies(const LearningEntry& entry, const std::u32string& leftContext,
                 bool atStart) const;

    // 候補ウィンドウ用: この読みで学習済みの変換結果を、文脈が合うものから新しい順に返す
    std::vector<std::u32string> LearnedResults(const std::u32string& reading,
                                               const std::u32string& leftContext) const;

private:
    std::vector<LearningEntry> entries_;
    // 読み全体 → 確定文字列
    std::unordered_map<std::u32string, LearningEntry> sentences_;
    // 読み → 文節エントリ（新しい順）
    std::unordered_map<std::u32string, std::vector<LearningEntry>> segments_;
    size_t maxSegmentReadingLength_ = 0;
};

} // namespace iroha
