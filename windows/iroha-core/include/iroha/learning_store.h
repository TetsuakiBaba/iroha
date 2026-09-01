#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "iroha/learning_dictionary.h"

namespace iroha {

// 変換の学習結果の永続化（JSONファイル。macOS版learning.jsonと同形式）。
// 移植元: macos/Sources/IrohaCore/LearningStore.swift
//
// 記録するのは「ユーザが変換を修正して確定した」ときだけで、
// エンジンの出力をそのまま確定した場合は何も覚えない（呼び出し側の責務）。
// スレッドセーフではない。変換サーバの単一スレッドから使うこと。
class LearningStore {
public:
    // 保持する上限（超えたら古いものから捨てる）
    static constexpr size_t kMaxSentences = 500;
    static constexpr size_t kMaxSegments = 2000;

    explicit LearningStore(std::filesystem::path path);

    const LearningDictionary& Current() const { return cached_; }
    size_t Count() const { return cached_.Entries().size(); }

    struct SegmentPair {
        std::u32string reading;
        std::u32string result;
    };

    // ユーザの修正を記録する。segmentsは確定時の文節（左からの並び順であること）
    void Record(const std::u32string& reading, const std::u32string& result,
                const std::vector<SegmentPair>& segments);

    void Reset();

private:
    void Merge(const std::vector<LearningEntry>& recorded);
    void Save(const std::vector<LearningEntry>& entries) const;

    std::filesystem::path path_;
    LearningDictionary cached_;
};

} // namespace iroha
