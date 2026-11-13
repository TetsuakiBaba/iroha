#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace iroha {

// 確定1件の記録（学習用データ）。移植元: macos/Sources/IrohaCore/ConversionLog.swift（形式を合わせること）。
//
// 学習に使うときの基本形は context + reading → committed。proposed と edited は
// 「モデルがどう間違えたか」（選好学習のペアや誤り分析）に使う
struct ConversionLogEntry {
    // どの操作で確定したか
    enum class Mode {
        Live,        // ライブ変換の表示をそのまま確定した
        Segments,    // 文節変換（スペースキー）で候補を選んで確定した
        FunctionKey, // F6〜F10 で指定した表示形（ひらがな・カタカナ・英数）で確定した
    };
    // 左文脈をどこから取ったか
    enum class ContextSource {
        Document,  // アプリのカーソル手前のテキスト
        Committed, // iroha が直前に確定した文字列の蓄積（アプリから読めないとき）
        None,      // どちらも無い
    };
    struct Segment {
        std::u32string reading;
        std::u32string result;
        bool operator==(const Segment& other) const {
            return reading == other.reading && result == other.result;
        }
    };

    // 記録の形式。項目を変えたら上げる（macOS版の ConversionLog.currentFormat と同じ値にする）
    static constexpr int kCurrentFormat = 1;

    int format = kCurrentFormat;
    // 1970-01-01 からの秒（UTC）
    int64_t timestamp = 0;
    Mode mode = Mode::Live;
    // エンジンに渡した左文脈（文書側の文脈 + 未確定文字列の固定部分）
    std::u32string context;
    ContextSource contextSource = ContextSource::None;
    // ひらがなの読み
    std::u32string reading;
    // エンジンが提示していた変換結果（不明なら nullopt）
    std::optional<std::u32string> proposed;
    // 確定された文字列
    std::u32string committed;
    // committed != proposed。proposed が不明なら nullopt
    std::optional<bool> edited;
    // 文節変換で確定したときの文節（左から）
    std::optional<std::vector<Segment>> segments;
    // 変換モデルのファイル名（拡張子なし）
    std::string model;

    // edited を proposed と committed から決めて作る
    static ConversionLogEntry Make(Mode mode, std::u32string context, ContextSource source,
                                   std::u32string reading, std::optional<std::u32string> proposed,
                                   std::u32string committed,
                                   std::optional<std::vector<Segment>> segments = std::nullopt);

    // 学習データとして中身が無い記録か（読みをそのまま確定した1文字。「、」「の」など）
    bool IsTrivial() const;

    // 1行の JSON（macOS版の JSONEncoder の出力と同じ: キーは名前順、無い項目は書かない、'/' はそのまま）
    std::string Encode() const;
    static std::optional<ConversionLogEntry> Decode(const std::string& line);

    bool operator==(const ConversionLogEntry& other) const;
};

// 何を記録するか（設定。macOS版の ConversionLogSettings.Scope）
enum class ConversionLogScope {
    All,         // すべての確定（修正しなかったものも含む）
    Corrections, // モデルの出力を直した確定だけ
};

// この確定を記録するか（設定が ON でプライベートモードでないことは呼び出し側が確かめる）。
// 左文脈がない確定・読みをそのまま確定した 1 文字は記録しない
bool ShouldRecordConversion(const ConversionLogEntry& entry, ConversionLogScope scope);

// 確定した変換を1件ずつ記録する追記専用のログ（将来の追加学習用のデータ）。
// - 1行1件の JSON（JSONL）。データフォルダの logs/conversions/ に月ごとのファイル
//   conversions-<コンピュータ名>-YYYY-MM.jsonl（macOS版と同じ。データフォルダを共有したときに同じファイルへ
//   複数台が追記しないように、機械ごとに分ける）
// - 左文脈にはユーザが書いていた文書の文章がそのまま入るので、記録は既定 OFF（設定で ON）
class ConversionLog {
public:
    ConversionLog(std::filesystem::path directory, std::string hostName);

    // 1件追記する
    void Record(const ConversionLogEntry& entry);

    // timestamp の月（ローカル時刻）のファイル
    std::filesystem::path FilePath(int64_t timestamp) const;
    // このフォルダにあるログファイル（全ホスト分。名前順）
    std::vector<std::filesystem::path> Files() const;
    uintmax_t TotalSize() const;

    // 記録1件と、それがどのファイルのどの行か（設定画面の一覧・削除用）
    struct Item {
        std::filesystem::path file;
        size_t line = 0;
        std::string original; // ファイル上の行（削除のときの照合に使う）
        ConversionLogEntry entry;
    };
    // すべての記録を時系列（古い順）で返す。同時刻はファイル名・行順
    std::vector<Item> Items() const;
    // まとめて削除する（ファイルごとに 1 回だけ書き戻す。行がずれていれば同じ内容の行を探す）
    void Delete(const std::vector<Item>& items);
    // すべてのログファイルを削除する（他のホストの分も含む）
    void RemoveAll();

    const std::filesystem::path& Directory() const { return directory_; }

private:
    std::filesystem::path directory_;
    std::string hostName_;
    mutable std::mutex mutex_;
};

} // namespace iroha
