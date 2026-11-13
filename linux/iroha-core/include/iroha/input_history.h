#pragma once
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace iroha {

// 入力履歴の1件: 読み → 確定した表記と、確定した回数・最後に確定した日時（1970-01-01 からの秒）。
// 移植元: macos/Sources/IrohaCore/InputHistory.swift（挙動互換を維持すること）。
// 予測変換（入力中に読みの先頭が一致する語を出す）はこれだけで作る
struct InputHistoryEntry {
    std::u32string reading;
    std::u32string text;
    int64_t count = 1;
    int64_t lastUsed = 0;

    bool operator==(const InputHistoryEntry& other) const {
        return reading == other.reading && text == other.text && count == other.count &&
               lastUsed == other.lastUsed;
    }
};

struct InputHistoryKey {
    std::u32string reading;
    std::u32string text;
    bool operator==(const InputHistoryKey& other) const {
        return reading == other.reading && text == other.text;
    }
    bool operator<(const InputHistoryKey& other) const {
        return reading != other.reading ? reading < other.reading : text < other.text;
    }
};

// 確定した「読み → 表記」から、入力履歴に残す単位を切り出す（文節ごとに、文節そのものと、
// 末尾のひらがなを除いた語の部分の両方。句読点は落とす）。
// ローマ字で打った読みだけを対象にする（読みにひらがな以外が混ざる文節・表記に英数字が混ざる文節は残さない）
class InputHistoryUnits {
public:
    // 残す単位の読みの最低文字数
    static constexpr size_t kMinimumReadingLength = 2;
    static std::vector<InputHistoryKey> Units(const std::u32string& reading, const std::u32string& text);
};

// 入力履歴の永続化と検索（予測変換の候補を引く）。
// - データフォルダの input-history/<ホスト名>.json に、その機械で確定した分だけを書く（macOS版と同じ形式）。
//   引くときは全ホストのファイルを合算する（回数は足し、最後に使った日時は新しい方）
// - 削除は他の機械のファイルを書き換えず、自分のファイルに「消した記録」を残す
// - 書き込みは確定を待たせないよう、まとめて少し遅らせる（saveDelayMs が 0 ならその場で書く）
class InputHistoryStore {
public:
    // 1台ぶんのファイルに残す上限（超えたら最後に使った日時が古いものから捨てる）
    static constexpr size_t kMaxEntries = 10000;
    // 「消した記録」の上限
    static constexpr size_t kMaxRemovals = 2000;
    // 予測に出す候補の数
    static constexpr size_t kCandidateLimit = 3;

    InputHistoryStore(std::filesystem::path directory, std::string hostName, int saveDelayMs = 3000);
    ~InputHistoryStore();
    InputHistoryStore(const InputHistoryStore&) = delete;
    InputHistoryStore& operator=(const InputHistoryStore&) = delete;

    // 確定した「読み → 表記」を記録する（切り出した単位ごとに回数を1つ増やす）。now は秒
    void Record(const std::u32string& reading, const std::u32string& text, int64_t now);

    // 読みが prefix で始まり、それより長い語を、確定した回数の多い順（同じなら最近使った順）に返す。
    // 表記が同じものは1つにまとめる
    std::vector<InputHistoryEntry> Candidates(const std::u32string& prefix,
                                              size_t limit = kCandidateLimit) const;

    // 全ホストを合算した一覧（設定画面用。並びは決めない）
    std::vector<InputHistoryEntry> Entries() const;
    size_t Count() const;

    // 指定した語を消す（他の機械で確定した分も、合算のときに数えなくなる）
    void Remove(const std::vector<InputHistoryKey>& keys, int64_t now);
    // すべて消す
    void Reset(int64_t now);

    // ファイルを読み直す（設定画面が書き換えたあと・他の機械の分が変わったとき）
    void Reload();
    // 溜まっている書き込みをすぐに済ませる
    void Flush();

    std::filesystem::path FilePath(const std::string& host) const;

    // 日時の文字列（ISO 8601、UTC。例 2026-11-13T10:00:00Z）と秒の変換
    static std::string FormatTime(int64_t seconds);
    static std::optional<int64_t> ParseTime(const std::string& text);
    static int64_t NowSeconds();

    struct Removal {
        std::u32string reading;
        std::u32string text;
        int64_t removedAt = 0;
    };
    struct HostFile {
        std::vector<InputHistoryEntry> entries;
        std::vector<Removal> removals;
        std::optional<int64_t> clearedAt;
        std::optional<int64_t> importedThrough;
    };

private:
    void RebuildMerged();
    void ScheduleSave();
    void SaveNow();
    void SaverLoop();
    static HostFile Load(const std::filesystem::path& path);
    static void Save(const HostFile& file, const std::filesystem::path& path);

    std::filesystem::path directory_;
    std::string hostName_;
    int saveDelayMs_;

    mutable std::mutex mutex_;
    HostFile own_;
    std::map<std::string, HostFile> others_;
    std::map<InputHistoryKey, InputHistoryEntry> merged_;
    // 読みの最初の 1 文字 → その文字で始まる語（候補を引く索引）
    std::unordered_map<char32_t, std::vector<InputHistoryEntry>> byFirstCharacter_;

    // 遅らせた書き込み
    std::condition_variable saveCondition_;
    bool dirty_ = false;
    bool stopping_ = false;
    std::thread saver_;
};

} // namespace iroha
