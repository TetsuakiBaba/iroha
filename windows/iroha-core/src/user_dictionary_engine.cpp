#include "iroha/user_dictionary_engine.h"

#include <algorithm>

namespace iroha {

UserDictionaryEngine::UserDictionaryEngine(ConversionEngine* base,
                                           DictionaryProvider dictionary)
    : base_(base), dictionaryProvider_(std::move(dictionary)) {}

bool UserDictionaryEngine::Convert(const std::u32string& reading,
                                   const std::u32string& context, int candidateCount,
                                   std::vector<std::u32string>* results,
                                   std::string* error) {
    results->clear();
    const UserDictionary dictionary = dictionaryProvider_();
    if (dictionary.IsEmpty()) {
        return base_->Convert(reading, context, candidateCount, results, error);
    }

    const std::vector<std::u32string> exactWords = dictionary.Words(reading);
    const std::vector<UserDictionary::Chunk> chunks = dictionary.Split(reading);
    const bool hasWordChunk =
        std::any_of(chunks.begin(), chunks.end(),
                    [](const UserDictionary::Chunk& c) { return c.isWord; });

    // ライブ変換・文節分割用（1候補）: 辞書を当てた結果をそのまま返す
    if (candidateCount <= 1) {
        if (!exactWords.empty()) {
            results->push_back(exactWords.front());
            return true;
        }
        if (!hasWordChunk) {
            return base_->Convert(reading, context, 1, results, error);
        }
        std::u32string composed;
        if (!Compose(chunks, context, &composed, error)) return false;
        results->push_back(composed);
        return true;
    }

    // 候補ウィンドウ用: ユーザ辞書の単語を先頭に、続けてエンジンの候補を並べる
    *results = exactWords;
    if (hasWordChunk && exactWords.empty()) {
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
