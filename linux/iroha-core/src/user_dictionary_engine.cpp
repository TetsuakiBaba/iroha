#include "iroha/user_dictionary_engine.h"
#include "iroha/inference_timer.h"

#include <algorithm>

namespace iroha {

namespace {

bool HasWordChunk(const std::vector<UserDictionary::Chunk>& chunks) {
    return std::any_of(chunks.begin(), chunks.end(),
                       [](const UserDictionary::Chunk& chunk) { return chunk.isWord; });
}

} // namespace

UserDictionaryEngine::UserDictionaryEngine(ConversionEngine* base,
                                           DictionaryProvider dictionary,
                                           LatticeConverter* lattice)
    : base_(base), dictionaryProvider_(std::move(dictionary)), lattice_(lattice) {}

bool UserDictionaryEngine::Convert(const std::u32string& reading,
                                   const std::u32string& context, int candidateCount,
                                   std::vector<std::u32string>* results,
                                   std::string* error) {
    if (!results) return false;
    results->clear();
    const UserDictionary dictionary = dictionaryProvider_();
    if (dictionary.IsEmpty()) {
        return base_->Convert(reading, context, candidateCount, results, error);
    }

    // ライブ変換・文節分割用（1候補）: 辞書を当てた結果をそのまま返す。
    // ただし出力が読みより大幅に長いエントリ（ハッシュタグ・定型文など）は
    // 文中に埋め込まれると邪魔なので使わない（候補ウィンドウにだけ出す）
    if (candidateCount <= 1) {
        if (dictionary.IsEmptyForLiveConversion()) {
            return base_->Convert(reading, context, 1, results, error);
        }
        const std::vector<std::u32string> liveWords = dictionary.LiveWords(reading);
        if (!liveWords.empty()) {
            results->push_back(liveWords.front());
            inference_timer::Note(InferenceStats::kUserDictionary);
            return true;
        }
        const std::optional<std::set<int>> boundaries = WordBoundaries(reading, dictionary, true);
        const std::vector<UserDictionary::Chunk> chunks =
            dictionary.Split(reading, 2, true, boundaries ? &*boundaries : nullptr);
        if (!HasWordChunk(chunks)) {
            return base_->Convert(reading, context, 1, results, error);
        }
        std::u32string composed;
        if (!Compose(chunks, context, &composed, error)) return false;
        results->push_back(composed);
        return true;
    }

    // 候補ウィンドウ用: ユーザ辞書の単語を先頭に、続けてエンジンの候補を並べる
    const std::vector<std::u32string> exactWords = dictionary.Words(reading);
    const std::optional<std::set<int>> boundaries = WordBoundaries(reading, dictionary, false);
    const std::vector<UserDictionary::Chunk> chunks =
        dictionary.Split(reading, 2, false, boundaries ? &*boundaries : nullptr);
    *results = exactWords;
    if (HasWordChunk(chunks) && exactWords.empty()) {
        std::u32string composed;
        if (!Compose(chunks, context, &composed, error)) return false;
        results->push_back(composed);
    }
    std::vector<std::u32string> baseCandidates;
    std::string baseError;
    if (base_->Convert(reading, context, candidateCount, &baseCandidates, &baseError)) {
        for (const auto& candidate : baseCandidates) {
            if (std::find(results->begin(), results->end(), candidate) ==
                results->end()) {
                results->push_back(candidate);
            }
        }
    } else if (results->empty()) {
        // エンジンが失敗してもユーザ辞書の単語だけは出す（無ければエラー伝播）
        if (error) *error = baseError;
        return false;
    }
    return true;
}

// 読みの中に一致するエントリを辞書ラティスに入れて変換したときの語の境目。
// 一致するエントリが無い・ラティスが無いときは nullopt（ラティスを呼ばない）
std::optional<std::set<int>> UserDictionaryEngine::WordBoundaries(
    const std::u32string& reading, const UserDictionary& dictionary, bool forLiveConversion) {
    if (!lattice_) return std::nullopt;
    const auto words = dictionary.EntriesOccurringIn(reading, 2, forLiveConversion);
    if (words.empty()) return std::nullopt;
    return lattice_->WordBoundaries(reading, words);
}

// 辞書一致部分はそのまま、それ以外はエンジンに変換させて連結する。
// 直前までの変換結果を次のチャンクの文脈として渡す
bool UserDictionaryEngine::Compose(const std::vector<UserDictionary::Chunk>& chunks,
                                   const std::u32string& context, std::u32string* out,
                                   std::string* error) {
    std::u32string result;
    std::u32string runningContext = context;
    for (const UserDictionary::Chunk& chunk : chunks) {
        if (chunk.isWord) {
            result += chunk.text;
            runningContext += chunk.text;
        } else {
            std::vector<std::u32string> converted;
            if (!base_->Convert(chunk.text, runningContext, 1, &converted, error)) {
                return false;
            }
            const std::u32string& piece =
                converted.empty() ? chunk.text : converted.front();
            result += piece;
            runningContext += piece;
        }
    }
    *out = result;
    return true;
}

} // namespace iroha
