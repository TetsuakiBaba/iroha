#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "iroha/learning_dictionary.h"

namespace iroha {

// 変換の学習結果の永続化（JSONファイル。macOS版learning.jsonと同形式）。
// 移植元: macos/Sources/IrohaCore/LearningStore.swift
//
// 記録するのは「ユーザが文節変換で修正して確定した」ときだけで、
// エンジンの出力をそのまま確定した場合は何も覚えない（呼び出し側の責務）。
// 1件は「入力の読み全体 → 確定文字列」と、直す前にエンジンが出していた結果（LearningEntry）。
//
// スレッドセーフではない。変換サーバの単一スレッドから使うこと。
// macOS版にある「ほかの端末が書いたファイルの取り込み（reloadIfChanged）」は未移植
// （Windows版はデータフォルダの同期にまだ対応していない）
class LearningStore {
public:
    // 保持する上限（超えたら古いものから捨てる）
    static constexpr size_t kMaxEntries = 500;

    explicit LearningStore(std::filesystem::path path);

    const LearningDictionary& Current() const { return cached_; }
    size_t Count() const { return cached_.Entries().size(); }

    // ユーザの修正を記録する。
    //
    // 覚えるのは入力の読み全体 → 確定文字列で、次に同じ読みを丸ごと入力し、エンジンが
    // replaced と同じ結果を出したときに再現する。
    // 同じ読みの「直す前」の記録がない古い学習は、この記録で置き換える
    // （条件のある学習と並べて残すと、条件に合わないときに古い方が差し替えてしまう）
    //   reading: 入力全体の読み（ひらがな）
    //   replaced: 直す前にエンジン（学習を除く）が出していた結果。nullopt は不明
    //   result: 確定された文字列
    void Record(const std::u32string& reading, const std::optional<std::u32string>& replaced,
                const std::u32string& result);

    // 学習による差し替えを取り消す（差し替えた結果を、ユーザがエンジンの結果に戻して確定したとき）。
    // エンジンが engineResult を出したときに差し替えるエントリのうち、結果が result のものを消す
    void Forget(const std::u32string& reading, const std::u32string& engineResult,
                const std::u32string& result);

    // 一覧を丸ごと置き換える（設定画面の編集用）。読み・結果が空のエントリは落とす
    void ReplaceAll(std::vector<LearningEntry> entries);

    void Reset();

    // base に recorded を重ねる。同じ読み・同じ「直す前」は新しい方（updatedAt）を採り、
    // 上限を超えたら古いものから捨てる
    static std::vector<LearningEntry> Merged(const std::vector<LearningEntry>& base,
                                             const std::vector<LearningEntry>& recorded);

private:
    void Store(std::vector<LearningEntry> entries);
    void Save(const std::vector<LearningEntry>& entries) const;

    std::filesystem::path path_;
    LearningDictionary cached_;
};

} // namespace iroha
