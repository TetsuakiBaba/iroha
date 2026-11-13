#pragma once
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "iroha/user_dictionary.h"

namespace iroha {

// ユーザ辞書の永続化（JSONファイル。macOS版user-dictionary.jsonと同形式）。
// 移植元: macos/Sources/IrohaCore/UserDictionaryStore.swift
//
// SyncFromSystemはmacOSではOSユーザ辞書からの取り込みに使う。Windowsに
// 対応物はないが、将来のMS-IMEユーザ辞書インポート等に流用できるため移植してある。
// スレッドセーフではない。変換サーバの単一スレッドから使うこと。
class UserDictionaryStore {
public:
    explicit UserDictionaryStore(std::filesystem::path path);

    const UserDictionary& Current() const { return cached_; }
    const std::vector<UserDictionaryEntry>& Entries() const {
        return cached_.Entries();
    }

    // 一覧をまるごと置き換えて保存する（設定UIの編集結果の反映）
    void ReplaceAll(std::vector<UserDictionaryEntry> entries);
    void Add(const std::u32string& reading, const std::u32string& word);

    // 取り込み結果のサマリ
    struct SyncResult {
        int added = 0;
        int removed = 0;
        int unchanged = 0;
        int skipped = 0; // 読みがひらがなでない等で対象外にしたもの
        bool operator==(const SyncResult& o) const {
            return added == o.added && removed == o.removed &&
                   unchanged == o.unchanged && skipped == o.skipped;
        }
    };

    // 外部辞書の内容に合わせる。.system由来のエントリだけを追加・削除の対象にし、
    // 手動追加した.manualのエントリには触らない（ミラーリング）
    SyncResult SyncFromSystem(
        const std::vector<std::pair<std::u32string, std::u32string>>& systemEntries);

private:
    void Store(std::vector<UserDictionaryEntry> entries);
    void Save(const std::vector<UserDictionaryEntry>& entries) const;

    std::filesystem::path path_;
    UserDictionary cached_;
};

} // namespace iroha
