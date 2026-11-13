#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace iroha {

// ユーザが変換を修正したときに記録する1エントリ
// （入力の読み全体 → 確定文字列。直す前にエンジンが出していた結果を条件に持つ）。
// 移植元: macos/Sources/IrohaCore/LearningDictionary.swift（JSON形式の互換を維持）
struct LearningEntry {
    // ひらがなの読み（変換した入力の全体）
    std::u32string reading;
    // 直す前にエンジン（学習を除く）が出していた変換結果。
    // 次にエンジンがこれと同じ結果を出したときだけ result に差し替える。
    // nullopt は記録なし（この条件を覚えるようになる前の学習）で、読みが一致すれば常に差し替える
    std::optional<std::u32string> replaced;
    // 確定された変換結果
    std::u32string result;
    // Unix時間（秒）。JSONではISO8601文字列
    int64_t updatedAt = 0;

    // 同じ読み・同じ「直す前」のエントリは1件にまとめる（新しい方を採る）
    bool SameKey(const LearningEntry& other) const {
        return reading == other.reading && replaced == other.replaced;
    }
    bool operator==(const LearningEntry& other) const {
        return SameKey(other) && result == other.result && updatedAt == other.updatedAt;
    }
};

// 変換時に参照する学習結果の不変スナップショット。
//
// 覚えるのは「入力の読み全体 → 確定した文字列」と、そのとき直す前にエンジンが出していた結果。
// 次に同じ読みを丸ごと入力し、エンジンが直す前と同じ結果を出したときにだけ差し替える。
// 左文脈が変わってエンジンの結果が変われば（「御恩と」の後の「ほうこう」→「奉公」など）、
// 差し替えずにエンジンの結果を使う。文脈そのものは覚えない（文節単位の学習は廃止した）
class LearningDictionary {
public:
    LearningDictionary() = default; // empty
    explicit LearningDictionary(std::vector<LearningEntry> entries);

    bool IsEmpty() const { return byReading_.empty(); }
    const std::vector<LearningEntry>& Entries() const { return entries_; }

    // 入力全体が過去に確定した読みと完全一致するエントリ（新しい順。同じ「直す前」は1件）
    const std::vector<LearningEntry>& EntriesForReading(const std::u32string& reading) const;

    // エンジンが engineResult を出したときに差し替えるエントリ（無ければnullptr）。
    // 「直す前」が engineResult と一致するものを採り、無ければ「直す前」の記録がないものを採る。
    // engineResult が nullptr なら「直す前」の記録がないものだけ
    const LearningEntry* Entry(const std::u32string& reading,
                               const std::u32string* engineResult) const;

private:
    std::vector<LearningEntry> entries_;
    std::unordered_map<std::u32string, std::vector<LearningEntry>> byReading_;
};

} // namespace iroha
