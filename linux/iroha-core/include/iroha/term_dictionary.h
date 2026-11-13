#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace iroha {

// 専門辞書（設定の「専門辞書」から入れる「読み → 語」の一覧）。
// 移植元: macos/Sources/IrohaCore/TermDictionary/（TermDictionary.swift・TermDictionaryCatalog.swift・
// TermDictionaryLookup.swift。挙動互換を維持すること）
//
// 使い道は候補ウィンドウだけ: 辞書ラティス（LatticeConverter）が候補を作るときに語として加わり、
// できた候補をモデルの対数確率で並べる。ライブ変換の第一候補（モデルの生成）は変えない。
// 辞書ラティスへの加え方は LatticeConverter::BuildTermIndex（macOS版は azooKey のユーザ辞書の形式に
// 書き出して読ませるが、Windows版は同じ探索ができる木をメモリに作る）。
//
// 辞書のファイルは UTF-8 のテキストで、1 行が「読み<TAB>語」（読みはひらがな）。「#」で始まる行は注記で飛ばす
// （macos/scripts/build-term-dictionaries.py が作る）

// 語の種類。辞書ラティスでの品詞（前後のつながりやすさ）に使う
enum class TermKind {
    kNoun,       // 用語（一般名詞）
    kPerson,     // 人名
    kPlace,      // 地名・駅名
    kProperNoun, // そのほかの固有名詞（神社・寺院など）
};

// カタログ・記録の値（"noun" / "person" / "place" / "properNoun"）との変換
std::optional<TermKind> ParseTermKind(const std::string& value);
const char* TermKindName(TermKind kind);

struct TermPair {
    std::u32string reading;
    std::u32string word;
};

// 辞書 1 つぶんの語と、その種類
struct TermDictionaryPack {
    TermKind kind = TermKind::kNoun;
    std::vector<TermPair> pairs;
};

class TermDictionary {
public:
    // 1文字の読みは文中のどこにでも当たるので入れない
    static constexpr size_t kMinimumReadingLength = 2;
    // ラティスでの評価値。ユーザ辞書の語を語の境目の判定に入れるときと同じ -10
    static constexpr float kLatticeValue = -10;

    // 辞書ファイルの中身を「読み・語」の組にする。形の合わない行は飛ばす
    static std::vector<TermPair> Parse(const std::string& utf8);
};

// 配布している専門辞書の一覧（models/term-dictionaries.json）。
// アプリに焼き込むのはカタログの URL だけで、辞書の追加・作り直しはカタログを更新するだけで済ませる。
// 辞書ごとにライセンスが違うので、ライセンスと出典もカタログに持たせる
struct TermDictionaryCatalog {
    struct File {
        std::string url;
        uint64_t bytes = 0;
        // 16進小文字の SHA-256。落としたあと必ず照合する
        std::string sha256;
    };
    // 出典の1件
    struct Source {
        std::u32string name;
        std::u32string holder; // 作成者・著作権者
        std::u32string license;
        std::string url;
        std::string licenseUrl; // 無いこともある
    };
    // 収録している語の例
    struct Sample {
        std::u32string word;
        std::u32string reading;
    };
    struct Dictionary {
        std::string id;
        std::u32string name;    // 設定画面に出す名前（「医学」「人名」）
        std::u32string group;   // 見出しにする、辞書のまとまり（「分野別の用語（JMdict）」）
        std::u32string summary; // 中身の説明（無ければ空）
        std::vector<Sample> samples;
        uint64_t entries = 0;
        File file;
        TermKind kind = TermKind::kNoun; // 無ければ用語
        std::u32string license;
        std::vector<Source> sources;
        std::string version; // 作った日（YYYYMMDD）
    };

    int formatVersion = 0;
    std::vector<Dictionary> dictionaries;

    static constexpr int kSupportedFormatVersion = 1;
    // カタログの場所。環境変数 IROHA_TERM_DICTIONARY_CATALOG で差し替えられる（開発・検証用。file:/// も可）
    static std::string DefaultUrl();

    // JSON を読む。読めなければ偽（error に理由）
    static bool Parse(const std::string& json, TermDictionaryCatalog* catalog, std::string* error);

    const Dictionary* Find(const std::string& id) const;
};

// 入れた専門辞書の置き場所と素性。
// <データフォルダ>/dictionaries/<ID>.tsv に辞書、<ID>.json に素性を置く（macOS版と同じ形式。
// 素性の JSON は macOS版の JSONEncoder の出力と互換）
class TermDictionaryInstall {
public:
    struct Record {
        std::string id;
        std::u32string name;
        std::u32string group;
        std::string version;
        std::string sha256; // 辞書ファイルの SHA-256（16進小文字）
        uint64_t entries = 0;
        TermKind kind = TermKind::kNoun;
        // 入れた時点のカタログにあったライセンスと出典（オフラインでも出せるように残す）
        std::u32string license;
        std::vector<TermDictionaryCatalog::Source> sources;
        std::string installedAt; // ISO 8601（例 2026-11-13T10:00:00Z）

        static Record From(const TermDictionaryCatalog::Dictionary& dictionary,
                           const std::string& installedAt);
    };

    static std::filesystem::path Directory(const std::filesystem::path& dataDirectory);
    static std::filesystem::path FilePath(const std::filesystem::path& dataDirectory,
                                          const std::string& id);
    static std::filesystem::path RecordPath(const std::filesystem::path& dataDirectory,
                                            const std::string& id);

    // ID はファイル名に使うので、英小文字・数字・ハイフンだけを受け付ける（カタログの値をパスに入れるため）
    static bool IsValidId(const std::string& id);

    // 入れている辞書（辞書ファイルと素性が揃っているもの）。ID の順
    static std::vector<Record> InstalledRecords(const std::filesystem::path& dataDirectory);
    static bool WriteRecord(const std::filesystem::path& dataDirectory, const Record& record);
    // 辞書を外す（素性を先に消すので、途中で失敗しても「入っている」とは見なされない）
    static bool Remove(const std::filesystem::path& dataDirectory, const std::string& id);
    // カタログの版と入れている版が違うか（作り直された辞書は入れ直す）
    static bool NeedsUpdate(const Record& record, const TermDictionaryCatalog::Dictionary& dictionary);
    // 入れている辞書の語を読む（records の順。読めない辞書は除く）
    static std::vector<TermDictionaryPack> LoadPacks(const std::filesystem::path& dataDirectory,
                                                     const std::vector<Record>& records);
    // 辞書の組を表す文字列（ID と SHA-256。変わっていなければ読み直さない判定に使う）
    static std::string Signature(const std::vector<Record>& records);

    // 素性の JSON（macOS版と同じキー）
    static std::string EncodeRecord(const Record& record);
    static std::optional<Record> DecodeRecord(const std::string& json);
};

// 候補が、入れている専門辞書のどれに載っている語かを引く（候補ウィンドウで辞書名の印を出すため）。
// 辞書ファイルは「読み<TAB>語」の行が読み・語の順（UTF-8 のバイト順）に並んでいるので、二分探索で引く
class TermDictionaryLookup {
public:
    struct Table {
        std::u32string label; // 印に出す名前（「工学」「地名」）
        std::string data;     // 辞書ファイルの中身
        size_t start = 0;     // 先頭の「#」行を除いた、最初の語の行の位置
    };

    TermDictionaryLookup() = default;
    explicit TermDictionaryLookup(std::vector<Table> tables) : tables_(std::move(tables)) {}

    // 入れている辞書のファイルを読む（読めないファイルは除く）
    static TermDictionaryLookup Load(const std::filesystem::path& dataDirectory,
                                     const std::vector<TermDictionaryInstall::Record>& records);

    bool Empty() const { return tables_.empty(); }

    // 文節の候補 candidate（読み reading）に含まれる専門辞書の語が載っている辞書の名前。
    // 候補が「辞書の語」か「辞書の語＋読みの残り（助詞などのかな）」のときに当てる。
    // 読みの長い語から試し、最初に当たった語の辞書をすべて返す。当たらなければ空
    std::vector<std::u32string> Labels(const std::u32string& reading,
                                       const std::u32string& candidate) const;

    // 印に出す名前。カタログの名前の括弧書きは除く（「地名（市区町村・町域）」→「地名」）
    static std::u32string Label(const std::u32string& name);
    // 「#」で始まる行（出典・ライセンスの注記）の後ろの位置
    static size_t BodyStart(const std::string& data);
    static bool Contains(const std::string& key, const Table& table);

private:
    std::vector<Table> tables_;
};

} // namespace iroha
