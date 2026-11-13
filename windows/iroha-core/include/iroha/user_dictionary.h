#pragma once
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace iroha {

// ユーザ辞書の1エントリ。読みはひらがなに正規化して保持する。
// 移植元: macos/Sources/IrohaCore/UserDictionary.swift（JSON形式の互換を維持）
struct UserDictionaryEntry {
    // エントリの出所。macOS版の.systemはOSユーザ辞書からの取り込みを意味する。
    // Windowsに対応物はないがJSON互換のためデータモデルは残す
    enum class Source { Manual, System };

    std::string id; // UUID文字列（大文字）
    std::u32string reading;
    std::u32string word;
    Source source = Source::Manual;
};

// 読みを正規化した新しいエントリを作る（Swift版UserDictionaryEntry.initに相当）
UserDictionaryEntry MakeUserDictionaryEntry(
    const std::u32string& reading, const std::u32string& word,
    UserDictionaryEntry::Source source = UserDictionaryEntry::Source::Manual);

// ランダムなUUID文字列（8-4-4-4-12、大文字）
std::string GenerateUuid();

// 変換時に参照する不変のスナップショット（読み→単語の索引つき）。
class UserDictionary {
public:
    // 読みを辞書一致部分とそれ以外に分けた結果
    struct Chunk {
        bool isWord;         // true: ユーザ辞書の単語（変換済み） / false: エンジンに渡す読み
        std::u32string text;
        bool operator==(const Chunk& other) const {
            return isWord == other.isWord && text == other.text;
        }
    };

    UserDictionary() = default; // empty
    explicit UserDictionary(std::vector<UserDictionaryEntry> entries);

    bool IsEmpty() const { return wordsByReading_.empty(); }
    // ライブ変換に使うエントリが1つも無いか（無ければ素通しでよい）
    bool IsEmptyForLiveConversion() const { return liveWordsByReading_.empty(); }
    const std::vector<UserDictionaryEntry>& Entries() const { return entries_; }

    // 読みに完全一致する単語（登録順）
    std::vector<std::u32string> Words(const std::u32string& reading) const;

    // 読みに完全一致する単語のうちライブ変換に使うもの（登録順）
    std::vector<std::u32string> LiveWords(const std::u32string& reading) const;

    // 読み（文節全体）の中に一致する、候補ウィンドウにだけ出す単語。
    //
    // これらは候補から選んで確定しても学習に記録しない。学習が覚えると、
    // ライブ変換から除外した意味がなくなる（学習エンジンは辞書の外側にあるため）。
    // 部分一致はSplitと同じくminimumMatchLength文字以上の読みだけを見る
    std::vector<std::u32string> CandidateOnlyWords(const std::u32string& reading,
                                                   int minimumMatchLength = 2) const;

    // 単語をライブ変換の結果として埋め込んでよいか。
    //
    // 出力が読みより大幅に長いエントリ（「たぐ」→「#helloworld #dummytag」、
    // 「めーる」→ メールアドレス、「おせわ」→ 定型文など）は文中に埋め込む語ではなく
    // 入力の省略記法なので、ライブ変換では使わず候補ウィンドウにだけ出す。
    // 漢字変換は読みより短くなるのが普通なので、長くなる方向だけを見る
    // （2倍を超え、かつ3文字以上長いものを除く。「かぶ」→「株式会社」程度の略記は通す）
    static bool IsSuitableForLiveConversion(const std::u32string& reading,
                                            const std::u32string& word);

    // 読みの中に部分一致する（Split が一致の対象にする）エントリの（読み, 単語）
    std::vector<std::pair<std::u32string, std::u32string>> EntriesOccurringIn(
        const std::u32string& reading, int minimumMatchLength = 2,
        bool forLiveConversion = false) const;

    // 読み全体を、ユーザ辞書に一致する部分とそれ以外に左から最長一致で分割する。
    //
    // 1文字の読み（「あ」等）が文中で無差別に一致すると変換が壊れるため、
    // 部分一致はminimumMatchLength文字以上のエントリだけを対象にする
    // （完全一致はWords()が長さに関係なく拾う）。
    // forLiveConversion が真ならライブ変換に使うエントリだけを一致の対象にする。
    //
    // wordBoundaries（読みの文字位置）を渡すと、始まりと終わりがどちらもその位置にある一致だけを使う。
    // 読みの並びが一致するだけでは語の途中にも当たる（「きえました」の「えま」→「き江間した」）ため、
    // 辞書ラティスの語の境目（LatticeConverter::WordBoundaries）で語として成り立つ所に限る
    std::vector<Chunk> Split(const std::u32string& reading, int minimumMatchLength = 2,
                             bool forLiveConversion = false,
                             const std::set<int>* wordBoundaries = nullptr) const;

    // 読みをひらがなに正規化する（カタカナ入力・前後の空白を吸収）
    static std::u32string NormalizedReading(const std::u32string& text);

    // irohaの読み（ローマ字入力の結果＝ひらがな）として成立する読みか
    static bool IsImportableReading(const std::u32string& text);

private:
    std::vector<UserDictionaryEntry> entries_;
    using Index = std::unordered_map<std::u32string, std::vector<std::u32string>>;
    // 読み → 単語（登録順、重複なし）。候補ウィンドウ用の全エントリ
    Index wordsByReading_;
    // 読み → 単語のうちライブ変換にも使うもの（IsSuitableForLiveConversion を満たす）
    Index liveWordsByReading_;
    // 読み → 単語のうち候補ウィンドウにだけ出すもの（学習の除外判定に使う）
    Index candidateOnlyWordsByReading_;
    // 索引にある読みの最大文字数（最長一致の走査上限）
    size_t maxReadingLength_ = 0;
};

} // namespace iroha
