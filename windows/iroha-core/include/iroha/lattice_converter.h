#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "iroha/term_dictionary.h"

namespace iroha {

// 専門辞書を辞書ラティスで引ける形にしたもの（LatticeConverter::BuildTermIndex で作る。中身は src/lattice/）
class TermLatticeIndex;

// 辞書ラティスによるかな漢字変換。
// 移植元: macos/Sources/IrohaCore/LatticeConverter.swift（挙動互換を維持すること）
//
// zenz（LLM）は自由生成なので、読みの合わない語（「ないようを」→「活用を」）を候補に出して
// しまうことがある。辞書ラティスは「読みが本当に一致する語の組み合わせ」しか作れないので、
// ここで作った候補をモデルで並べ替えれば、読みを保証したまま文脈を効かせられる。
//
// macOS 版は AzooKeyKanaKanjiConverter（Swift、MIT）を使う。Windows 版はその変換の部分を
// C++ に移したもの（src/lattice/）を使い、同じ辞書データ（azooKey_dictionary_storage、Apache-2.0）を読む。
//
// 読みは NFC のかな（結合濁点などを含む入力は、境界で正規化してから渡すこと）。
// スレッドセーフではない（macOS 版は actor で直列化している。呼び出し側で直列化すること）
class LatticeConverter {
public:
    // ラティスが返した1候補
    struct Candidate {
        std::u32string text;
        // ラティスの評価値（大きいほど良い。対数確率に相当するスケール）
        double value = 0;
        // 読み全体を変換した候補か（false なら先頭文節だけなどの部分候補）
        bool isFullMatch = false;

        bool operator==(const Candidate& other) const {
            return text == other.text && value == other.value && isFullMatch == other.isFullMatch;
        }
    };

    // 辞書フォルダの既定の探索先。見つからなければnullopt（ラティスなしで動く）
    //
    // 1. 環境変数 `IROHA_DICTIONARY`
    // 2. `executableDirectory`（実行ファイルのあるフォルダ。空なら見ない）の `Dictionary`（配布物の配置）
    // 3. カレントディレクトリから上へたどった `vendor/azooKey_dictionary_storage/Dictionary`
    //    （iroha-cli・テストをリポジトリの中から実行したとき）
    static std::optional<std::filesystem::path> DefaultDictionaryPath(
        const std::filesystem::path& executableDirectory = {});

    // 辞書フォルダとして使えるか（必須ファイルの有無で判定）
    static bool IsDictionaryDirectory(const std::filesystem::path& path);

    explicit LatticeConverter(const std::filesystem::path& dictionaryPath);
    ~LatticeConverter();
    LatticeConverter(const LatticeConverter&) = delete;
    LatticeConverter& operator=(const LatticeConverter&) = delete;

    // 読み全体に一致する候補（ラティスの評価順）。最大 `count` 件
    std::vector<std::u32string> Candidates(const std::u32string& reading, int count);

    // 読み全体に一致する候補をすべて返す（ラティスの評価順、重複なし）。
    //
    // 先頭に文全体のn-best、その後ろに読みが一致する辞書エントリ
    // （単漢字・異体字・人名など。評価値は低いが読みは正しい）が続く。
    // 候補ウィンドウでスクロールして辿れるよう、上限は `kMaxFullMatchCandidates`
    std::vector<std::u32string> FullMatchCandidates(const std::u32string& reading, int nBest);

    // `FullMatchCandidates` が返す候補数の上限
    static constexpr int kMaxFullMatchCandidates = 60;

    // ラティスの返した候補をそのまま（部分候補も含む。調査・デバッグ用）
    std::vector<Candidate> RawCandidates(const std::u32string& reading, int count);

    // 専門辞書を辞書ラティスで引ける形にする（数十万語で 1 秒前後かかりうるので、変換と別のスレッドで作ってよい。
    // 作ったものは変更しないので、複数の LatticeConverter で共有できる）。
    // `dictionaryPath` は本体の辞書のフォルダ（文字 ID の表を合わせる）
    static std::shared_ptr<const TermLatticeIndex> BuildTermIndex(
        const std::filesystem::path& dictionaryPath, const std::vector<TermDictionaryPack>& packs);
    // 入れた語の数
    static size_t TermIndexEntryCount(const TermLatticeIndex& index);

    // 候補を作るときに加える専門辞書（nullptr で外す）。ユーザ辞書の語の境目の判定（WordBoundaries）には入れない
    // （ユーザ辞書をライブ変換に当てる位置を、専門辞書で変えない。macOS版と同じ）
    void SetTermDictionary(std::shared_ptr<const TermLatticeIndex> index);

    // ユーザ辞書の語（読み, 語）を加えたラティスで読み全体を変換し、評価が最も良い候補の語の境目を返す。
    //
    // 値は読みの文字位置（先頭の 0 と末尾の文字数を含む）。ユーザ辞書の語を読みの途中に当ててよいかを
    // 決めるのに使う。語は固有名詞・評価値 -10 で入れるので、語として前後とつながる場所
    // （「えまとあそぶ」）では最良の候補に残り、動詞などの途中（「きえました」の「えま」）では
    // 品詞の連接で負けて境目が合わない。語はこの呼び出しの間だけ入れる。
    // 読み全体の候補が無ければ nullopt
    std::optional<std::set<int>> WordBoundaries(
        const std::u32string& reading,
        const std::vector<std::pair<std::u32string, std::u32string>>& userWords);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iroha
