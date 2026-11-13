#pragma once
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace iroha {

// 1 つの読みに対する記号・顔文字・絵文字の候補（SymbolCandidateTable が返す）
struct SymbolCandidates {
    std::vector<std::u32string> emoji;
    std::vector<std::u32string> emoticons;
    std::vector<std::u32string> symbols;

    bool Empty() const { return emoji.empty() && emoticons.empty() && symbols.empty(); }
    // 絵文字 → 顔文字 → 記号 の順にすべて
    std::vector<std::u32string> All() const;
    bool operator==(const SymbolCandidates& other) const {
        return emoji == other.emoji && emoticons == other.emoticons && symbols == other.symbols;
    }
};

// 読みから記号・顔文字・絵文字を引く表。
// 移植元: macos/Sources/IrohaCore/SymbolCandidates/SymbolCandidateTable.swift（挙動互換を維持すること）
//
// データは Mozc（BSD 3-Clause、Google）の src/data/ の 4 ファイル（symbol.tsv・emoticon.tsv・
// categorized.tsv・emoji_data.tsv）。macOS版と同じコミットのものを windows/scripts/fetch-symbols.ps1 が
// vendor/mozc-data に取得し、インストール時に実行ファイルと同じフォルダの Symbols へコピーする。
// 候補ウィンドウにだけ合流させ（SymbolCandidateMerger）、ライブ変換には使わない。
// 読みは Mozc に合わせて「えもじ」で全部の絵文字、「かおもじ」で全部の顔文字を返す
class SymbolCandidateTable {
public:
    // OS が描ける絵文字か。描けないものは □ や部品の並びになるので候補に出さない
    using RenderablePredicate = std::function<bool(const std::u32string&)>;

    static constexpr char32_t kAllEmojiReading[] = U"えもじ";
    static constexpr char32_t kAllEmoticonsReading[] = U"かおもじ";
    // データのファイル名（Load が読む順）
    static const std::vector<std::string>& FileNames();

    // 各 TSV の中身（UTF-8）から作る。isRenderableEmoji が空ならすべて描けるものとする
    SymbolCandidateTable(const std::string& symbolTsv, const std::string& emoticonTsv,
                         const std::string& categorizedTsv, const std::string& emojiTsv,
                         const RenderablePredicate& isRenderableEmoji = nullptr);

    // directory の 4 ファイルから作る。1 つでも読めなければ nullopt
    static std::optional<SymbolCandidateTable> Load(const std::filesystem::path& directory,
                                                    const RenderablePredicate& isRenderableEmoji = nullptr);

    // データの置き場所: 環境変数 IROHA_SYMBOLS → 実行ファイルと同じフォルダの Symbols →
    // カレントから 4 階層上までの vendor/mozc-data（iroha-cli・テスト用）
    static std::optional<std::filesystem::path> DefaultDirectory(
        const std::filesystem::path& executableDirectory);

    // 読み（ひらがな。カタカナでもよい）に一致する候補。各種類の中はデータの並び順
    SymbolCandidates Candidates(const std::u32string& reading) const;

    // 候補の数（iroha-cli symbols の表示用）
    struct Counts {
        size_t emoji = 0;
        size_t emoticons = 0;
        size_t symbols = 0;
    };
    Counts GetCounts() const;

private:
    using Index = std::unordered_map<std::u32string, std::vector<std::u32string>>;
    static void Add(const std::u32string& value, const std::u32string& readings, Index* table,
                    std::vector<std::u32string>* all);

    Index emoji_;
    Index emoticons_;
    Index symbols_;
    std::vector<std::u32string> allEmoji_;
    std::vector<std::u32string> allEmoticons_;
};

// 記号・顔文字・絵文字の候補を文節の候補ウィンドウに合流させる規則。
// 移植元: macos/Sources/IrohaCore/SymbolCandidates/SymbolCandidateMerger.swift
//
// - 絵文字は上から 7 番目に入れる（Mozc の EmojiRewriter の kDefaultInsertPos = 6 と同じ）。
//   先頭の動かさない候補（今の表示など）がそれより下まで来ていれば、その後ろに入れる。
//   エンジン（辞書）が既に出している絵文字は、その位置から絵文字の並びへ移す
// - 顔文字・記号は、エンジンの候補の後ろに 顔文字 → 記号 の順で足す。既に出ているものは元の位置のまま
// - ライブ変換には使わない（候補ウィンドウを開くときだけ使う）
class SymbolCandidateMerger {
public:
    // 絵文字を入れる位置（0 始まり）
    static constexpr size_t kEmojiPosition = 6;

    // candidates の先頭 protectedCount 件は動かさない
    static std::vector<std::u32string> Merge(const std::vector<std::u32string>& candidates,
                                             const SymbolCandidates& symbols,
                                             size_t protectedCount);

    // 確定しても学習しない候補: 表に載っていて、この読みで出た候補すべて（エンジンが出していたものも含む）。
    // ただし漢字・かなだけでできた候補は除く（Mozc の記号データには部首の「女」のような普通の文字も入っていて、
    // それらは普通の語と同じく学習してよい）
    static std::set<std::u32string> UnlearnableCandidates(
        const std::vector<std::u32string>& candidates, const SymbolCandidates& symbols);

    // 漢字・ひらがな・カタカナ（長音符・踊り字を含む）だけでできているか
    static bool IsKanaOrKanji(const std::u32string& text);
};

} // namespace iroha
