#pragma once
// 配布している評価セットの一覧（models/benchmarks.json）と、入れた評価セットの置き場所と素性。
// 移植元: macos/Sources/IrohaCore/Benchmark/BenchmarkCatalog.swift（形式は macOS版と同じ）
//
// 専門辞書と同じく、アプリに焼き込むのはカタログの URL だけにする。評価セットの本体は GitHub Releases の
// プレリリースのタグ `benchmarks-v1` に置く（macos/scripts/publish-benchmarks.sh）。iroha-bench-v1 は CC BY-SA 4.0 で、
// 項目によっては CC BY 4.0・MIT の出典も混ざるので、本体のコード（MIT）とは分けて配り、ライセンスと出典はカタログに持たせる。
// 評価セットの中身を変えたら ID を変える（同じ ID の結果どうしを比べるため）。
// 取得（HTTP・SHA-256 の照合）は libcurl を使うので shared/benchmark_fetcher にある。文字列は UTF-8

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace iroha {

struct BenchmarkCatalog {
    struct File {
        std::string url;
        uint64_t bytes = 0;
        // 16進小文字の SHA-256。落としたあと必ず照合する
        std::string sha256;
    };
    // 出典の1件（設定 > 情報 のライセンス一覧に1行で出す）
    struct Source {
        std::string name;
        std::string holder;
        std::string license;
        std::string url;
        bool operator==(const Source& other) const {
            return name == other.name && holder == other.holder && license == other.license && url == other.url;
        }
    };
    struct Entry {
        std::string id;
        // 設定画面に出す名前
        std::string name;
        // 中身の説明（「?」に出す。無ければ空）
        std::string summary;
        // 項目の数
        int items = 0;
        File file;
        // 評価セット全体のライセンス
        std::string license;
        std::vector<Source> sources;
        // 出典の条文が写しに添えることを求める注記（MIT の著作権表示と許諾文など）。入れたときに素性へ写す
        std::optional<std::string> notice;
    };

    int formatVersion = 0;
    std::vector<Entry> benchmarks;

    static constexpr int kSupportedFormatVersion = 1;
    // カタログの場所。環境変数 IROHA_BENCHMARK_CATALOG で差し替えられる（開発・検証用。file:/// も可）
    static std::string DefaultUrl();
    // JSON を読む。読めない・形式が新しすぎるなら偽（error に理由）
    static bool Parse(const std::string& json, BenchmarkCatalog* catalog, std::string* error);

    const Entry* Find(const std::string& id) const;
};

// 入れた評価セット。<データフォルダ>/benchmarks/<ID>.jsonl に評価セット、<ID>.json に素性（カタログの項目と入れた日時）
class BenchmarkInstall {
public:
    struct Record {
        std::string id;
        std::string name;
        int items = 0;
        std::string sha256;
        // 入れた時点のカタログにあったライセンスと出典。オフラインでも 設定 > 情報 に出せるように残す
        std::string license;
        std::vector<BenchmarkCatalog::Source> sources;
        std::optional<std::string> notice;
        std::string installedAt; // ISO 8601（例 2026-11-14T10:00:00Z）

        static Record From(const BenchmarkCatalog::Entry& entry, const std::string& installedAt);
    };

    static std::filesystem::path Directory(const std::filesystem::path& dataDirectory);
    static std::filesystem::path FilePath(const std::filesystem::path& dataDirectory, const std::string& id);
    static std::filesystem::path RecordPath(const std::filesystem::path& dataDirectory, const std::string& id);
    // ID はファイル名に使うので、英小文字・数字・ハイフンだけを受け付ける
    static bool IsValidId(const std::string& id);
    // 入れている評価セット（ファイルと素性が揃っているもの）。ID の順
    static std::vector<Record> InstalledRecords(const std::filesystem::path& dataDirectory);
    static bool WriteRecord(const std::filesystem::path& dataDirectory, const Record& record);
    static bool Remove(const std::filesystem::path& dataDirectory, const std::string& id);
    // 入れている版がカタログと違う（作り直された）か
    static bool IsOutdated(const Record& record, const BenchmarkCatalog::Entry& entry);

    // 素性の JSON（macOS版と同じキー）
    static std::string EncodeRecord(const Record& record);
    static std::optional<Record> DecodeRecord(const std::string& json);
};

} // namespace iroha
