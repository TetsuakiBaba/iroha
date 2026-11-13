#include "iroha/variant_kanji_engine.h"

#include <algorithm>
#include <unordered_map>

#include "iroha/user_dictionary.h"

namespace iroha {

namespace {

// 単独では辞書に入りにくい人名用の異体字（旧字体・俗字）の表。
// 通常の字体（高・崎・徳…）は辞書とモデルが出すので載せない。
// macOS版の VariantKanjiTable と同じ内容にする
const std::unordered_map<std::u32string, std::vector<std::u32string>>& Table() {
    static const std::unordered_map<std::u32string, std::vector<std::u32string>> table = {
        {U"たか", {U"髙"}},
        {U"こう", {U"髙"}},
        {U"さき", {U"﨑", U"嵜"}},
        {U"ざき", {U"﨑", U"嵜"}},
        {U"とく", {U"德"}},
        {U"はま", {U"濵", U"濱"}},
        {U"よし", {U"𠮷"}},
        {U"さわ", {U"澤"}},
        {U"ざわ", {U"澤"}},
        {U"たき", {U"瀧"}},
        {U"ひろ", {U"廣"}},
        {U"さい", {U"齋", U"齊"}},
        {U"しぶ", {U"澁"}},
        {U"なべ", {U"邊", U"邉"}},
        {U"べ", {U"邊", U"邉"}},
        {U"くに", {U"國"}},
        {U"ま", {U"眞"}},
        {U"しん", {U"眞"}},
        {U"やなぎ", {U"栁"}},
        {U"かめ", {U"龜"}},
        {U"いわ", {U"巖"}},
        {U"しま", {U"嶌"}},
        {U"つち", {U"圡"}},
        {U"とみ", {U"冨"}},
        {U"くろ", {U"黑"}},
        {U"えい", {U"榮"}},
        {U"さくら", {U"櫻"}},
        {U"つる", {U"鶴", U"靍", U"靏"}},
        {U"はし", {U"槗"}},
        {U"みね", {U"峯"}},
        {U"ふじ", {U"冨士"}},
        {U"いち", {U"壹"}},
        {U"まん", {U"萬"}},
        {U"りゅう", {U"龍", U"竜"}},
        {U"よう", {U"樣"}},
    };
    return table;
}

} // namespace

std::vector<std::u32string> VariantKanjiEngine::Variants(const std::u32string& reading) {
    const auto& table = Table();
    const auto it = table.find(UserDictionary::NormalizedReading(reading));
    return it == table.end() ? std::vector<std::u32string>() : it->second;
}

bool VariantKanjiEngine::Convert(const std::u32string& reading, const std::u32string& context,
                                 int candidateCount, std::vector<std::u32string>* results,
                                 std::string* error) {
    if (!base_->Convert(reading, context, candidateCount, results, error)) return false;
    if (candidateCount <= 1) return true;
    for (const std::u32string& variant : Variants(reading)) {
        if (std::find(results->begin(), results->end(), variant) == results->end()) {
            results->push_back(variant);
        }
    }
    return true;
}

} // namespace iroha
