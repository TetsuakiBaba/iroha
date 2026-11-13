#pragma once
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "iroha/conversion_engine.h"

namespace iroha {

// 長い読みを区切って順に変換する、変換エンジンのデコレータ。
// 移植元: macos/Sources/IrohaCore/ChunkedConversionEngine.swift（挙動互換を維持すること）
//
// モデルはおおむね80文字を超える読みで途中や末尾を飛ばし始める。読み制約で終端を止めても、
// モデル自身が読みを追えなくなると意味のない文字列を出すだけなので、モデルに渡す読みの長さを
// maxChunkLength 以下に保つ（長さの根拠は macOS版のコメントにある実測）。
//
// 区切り方（先頭から順に決める）:
// 1. 先頭 maxChunkLength 文字の窓の中に句読点があれば、その直後で切る
// 2. なければ窓をいったん変換し、ReadingAligner の文節境界のうち最後の文節を除いた
//    位置で切る（窓の末尾は語の途中で切れている可能性があるため）。この変換結果は
//    その区切りの結果としてそのまま使う
// 3. 境界が見つからなければ窓の長さで切る
//
// 各区切りは直前までの変換結果を左文脈にして変換する。区切りの変換結果は
// （読み, 文脈）をキーにキャッシュするので、ライブ変換で末尾に1文字ずつ足していく間、
// 打鍵ごとに実際に再変換されるのは末尾の区切りだけになる。
//
// maxChunkLength 以下の読みはそのまま素通しする。
// 文字数はコードポイントで数える（読みはNFCのかなである前提。ReadingAligner と同じ）
class ChunkedConversionEngine : public ConversionEngine {
public:
    // モデルに渡す読みの最大文字数
    static constexpr size_t kDefaultMaxChunkLength = 80;

    // base は所有しない（呼び出し側がこのエンジンより長く生かすこと）
    explicit ChunkedConversionEngine(ConversionEngine* base,
                                     size_t maxChunkLength = kDefaultMaxChunkLength);

    bool Convert(const std::u32string& reading, const std::u32string& context,
                 int candidateCount, std::vector<std::u32string>* results,
                 std::string* error) override;

    // 窓の中で最後に現れる句読点の直後の位置（minimum以上のもの）。なければnullopt
    static std::optional<size_t> PunctuationCut(const std::u32string& window, size_t minimum);

private:
    // 窓（maxChunkLength文字）をどこで切るかと、その区切りの変換結果を返す
    bool CutWindow(const std::u32string& window, const std::u32string& context, size_t* cut,
                   std::u32string* converted, std::string* error);
    bool ConvertCached(const std::u32string& reading, const std::u32string& context,
                       std::u32string* converted, std::string* error);

    using CacheKey = std::pair<std::u32string, std::u32string>; // 読み, 文脈

    ConversionEngine* base_;
    size_t maxChunkLength_;
    // 区切りをこれより短くはしない（句読点や文節境界が窓の先頭付近にしかないとき）
    size_t minChunkLength_;
    // 区切りの変換結果のキャッシュ。古いものから捨てる
    std::map<CacheKey, std::u32string> cache_;
    std::deque<CacheKey> cacheOrder_;
    static constexpr size_t kCacheLimit = 64;
};

} // namespace iroha
