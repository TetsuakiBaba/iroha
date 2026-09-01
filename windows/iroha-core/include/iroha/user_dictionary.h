#pragma once
#include <string>
#include <unordered_map>
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
    const std::vector<UserDictionaryEntry>& Entries() const { return entries_; }

    // 読みに完全一致する単語（登録順）
    std::vector<std::u32string> Words(const std::u32string& reading) const;

    // 読み全体を、ユーザ辞書に一致する部分とそれ以外に左から最長一致で分割する。
    // 1文字の読み（「あ」等）が文中で無差別に一致すると変換が壊れるため、
    // 部分一致はminimumMatchLength文字以上のエントリだけを対象にする
    // （完全一致はWords()が長さに関係なく拾う）。
    std::vector<Chunk> Split(const std::u32string& reading,
                             int minimumMatchLength = 2) const;

    // 読みをひらがなに正規化する（カタカナ入力・前後の空白を吸収）
    static std::u32string NormalizedReading(const std::u32string& text);

    // irohaの読み（ローマ字入力の結果＝ひらがな）として成立する読みか
    static bool IsImportableReading(const std::u32string& text);

private:
    std::vector<UserDictionaryEntry> entries_;
    // 読み → 単語（登録順、重複なし）
    std::unordered_map<std::u32string, std::vector<std::u32string>> wordsByReading_;
    // 索引にある読みの最大文字数（最長一致の走査上限）
    size_t maxReadingLength_ = 0;
};

} // namespace iroha
