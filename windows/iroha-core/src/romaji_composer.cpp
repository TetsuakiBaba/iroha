#include "iroha/romaji_composer.h"

namespace iroha {

namespace {

const std::unordered_map<std::u32string, std::u32string>& DefaultTable() {
    static const std::unordered_map<std::u32string, std::u32string> table = {
        // 母音
        {U"a", U"あ"}, {U"i", U"い"}, {U"u", U"う"}, {U"e", U"え"}, {U"o", U"お"},
        // か行
        {U"ka", U"か"}, {U"ki", U"き"}, {U"ku", U"く"}, {U"ke", U"け"}, {U"ko", U"こ"},
        {U"kya", U"きゃ"}, {U"kyi", U"きぃ"}, {U"kyu", U"きゅ"}, {U"kye", U"きぇ"}, {U"kyo", U"きょ"},
        {U"ga", U"が"}, {U"gi", U"ぎ"}, {U"gu", U"ぐ"}, {U"ge", U"げ"}, {U"go", U"ご"},
        {U"gya", U"ぎゃ"}, {U"gyu", U"ぎゅ"}, {U"gyo", U"ぎょ"},
        // さ行
        {U"sa", U"さ"}, {U"si", U"し"}, {U"su", U"す"}, {U"se", U"せ"}, {U"so", U"そ"},
        {U"sha", U"しゃ"}, {U"shi", U"し"}, {U"shu", U"しゅ"}, {U"she", U"しぇ"}, {U"sho", U"しょ"},
        {U"sya", U"しゃ"}, {U"syi", U"しぃ"}, {U"syu", U"しゅ"}, {U"sye", U"しぇ"}, {U"syo", U"しょ"},
        {U"za", U"ざ"}, {U"zi", U"じ"}, {U"zu", U"ず"}, {U"ze", U"ぜ"}, {U"zo", U"ぞ"},
        {U"ja", U"じゃ"}, {U"ji", U"じ"}, {U"ju", U"じゅ"}, {U"je", U"じぇ"}, {U"jo", U"じょ"},
        {U"jya", U"じゃ"}, {U"jyu", U"じゅ"}, {U"jyo", U"じょ"},
        {U"zya", U"じゃ"}, {U"zyi", U"じぃ"}, {U"zyu", U"じゅ"}, {U"zye", U"じぇ"}, {U"zyo", U"じょ"},
        // た行
        {U"ta", U"た"}, {U"ti", U"ち"}, {U"tu", U"つ"}, {U"te", U"て"}, {U"to", U"と"},
        {U"cha", U"ちゃ"}, {U"chi", U"ち"}, {U"chu", U"ちゅ"}, {U"che", U"ちぇ"}, {U"cho", U"ちょ"},
        {U"tya", U"ちゃ"}, {U"tyi", U"ちぃ"}, {U"tyu", U"ちゅ"}, {U"tye", U"ちぇ"}, {U"tyo", U"ちょ"},
        {U"tsu", U"つ"}, {U"tsa", U"つぁ"}, {U"tsi", U"つぃ"}, {U"tse", U"つぇ"}, {U"tso", U"つぉ"},
        {U"tha", U"てゃ"}, {U"thi", U"てぃ"}, {U"thu", U"てゅ"}, {U"the", U"てぇ"}, {U"tho", U"てょ"},
        {U"twu", U"とぅ"},
        {U"da", U"だ"}, {U"di", U"ぢ"}, {U"du", U"づ"}, {U"de", U"で"}, {U"do", U"ど"},
        {U"dya", U"ぢゃ"}, {U"dyu", U"ぢゅ"}, {U"dyo", U"ぢょ"},
        {U"dha", U"でゃ"}, {U"dhi", U"でぃ"}, {U"dhu", U"でゅ"}, {U"dhe", U"でぇ"}, {U"dho", U"でょ"},
        {U"dwu", U"どぅ"},
        // な行
        {U"na", U"な"}, {U"ni", U"に"}, {U"nu", U"ぬ"}, {U"ne", U"ね"}, {U"no", U"の"},
        {U"nya", U"にゃ"}, {U"nyi", U"にぃ"}, {U"nyu", U"にゅ"}, {U"nye", U"にぇ"}, {U"nyo", U"にょ"},
        {U"nn", U"ん"}, {U"n'", U"ん"},
        // は行
        {U"ha", U"は"}, {U"hi", U"ひ"}, {U"hu", U"ふ"}, {U"he", U"へ"}, {U"ho", U"ほ"},
        {U"hya", U"ひゃ"}, {U"hyu", U"ひゅ"}, {U"hyo", U"ひょ"},
        {U"fa", U"ふぁ"}, {U"fi", U"ふぃ"}, {U"fu", U"ふ"}, {U"fe", U"ふぇ"}, {U"fo", U"ふぉ"},
        {U"fya", U"ふゃ"}, {U"fyu", U"ふゅ"}, {U"fyo", U"ふょ"},
        {U"ba", U"ば"}, {U"bi", U"び"}, {U"bu", U"ぶ"}, {U"be", U"べ"}, {U"bo", U"ぼ"},
        {U"bya", U"びゃ"}, {U"byu", U"びゅ"}, {U"byo", U"びょ"},
        {U"pa", U"ぱ"}, {U"pi", U"ぴ"}, {U"pu", U"ぷ"}, {U"pe", U"ぺ"}, {U"po", U"ぽ"},
        {U"pya", U"ぴゃ"}, {U"pyu", U"ぴゅ"}, {U"pyo", U"ぴょ"},
        // ま行
        {U"ma", U"ま"}, {U"mi", U"み"}, {U"mu", U"む"}, {U"me", U"め"}, {U"mo", U"も"},
        {U"mya", U"みゃ"}, {U"myu", U"みゅ"}, {U"myo", U"みょ"},
        // や行
        {U"ya", U"や"}, {U"yu", U"ゆ"}, {U"yo", U"よ"}, {U"ye", U"いぇ"},
        // ら行
        {U"ra", U"ら"}, {U"ri", U"り"}, {U"ru", U"る"}, {U"re", U"れ"}, {U"ro", U"ろ"},
        {U"rya", U"りゃ"}, {U"ryu", U"りゅ"}, {U"ryo", U"りょ"},
        // わ行
        {U"wa", U"わ"}, {U"wi", U"うぃ"}, {U"wu", U"う"}, {U"we", U"うぇ"}, {U"wo", U"を"},
        {U"wha", U"うぁ"}, {U"whi", U"うぃ"}, {U"whe", U"うぇ"}, {U"who", U"うぉ"},
        // ゔ
        {U"va", U"ゔぁ"}, {U"vi", U"ゔぃ"}, {U"vu", U"ゔ"}, {U"ve", U"ゔぇ"}, {U"vo", U"ゔぉ"},
        // c/q 系（Google日本語入力互換）
        {U"ca", U"か"}, {U"ci", U"し"}, {U"cu", U"く"}, {U"ce", U"せ"}, {U"co", U"こ"},
        {U"qa", U"くぁ"}, {U"qi", U"くぃ"}, {U"qu", U"く"}, {U"qe", U"くぇ"}, {U"qo", U"くぉ"},
        // 小書き
        {U"la", U"ぁ"}, {U"li", U"ぃ"}, {U"lu", U"ぅ"}, {U"le", U"ぇ"}, {U"lo", U"ぉ"},
        {U"xa", U"ぁ"}, {U"xi", U"ぃ"}, {U"xu", U"ぅ"}, {U"xe", U"ぇ"}, {U"xo", U"ぉ"},
        {U"ltu", U"っ"}, {U"xtu", U"っ"}, {U"ltsu", U"っ"},
        {U"lya", U"ゃ"}, {U"lyu", U"ゅ"}, {U"lyo", U"ょ"},
        {U"xya", U"ゃ"}, {U"xyu", U"ゅ"}, {U"xyo", U"ょ"},
        {U"lwa", U"ゎ"}, {U"xwa", U"ゎ"},
        {U"xka", U"ヵ"}, {U"xke", U"ヶ"},
        // 記号
        {U"-", U"ー"}, {U",", U"、"}, {U".", U"。"}, {U"/", U"・"},
        {U"[", U"「"}, {U"]", U"」"}, {U"!", U"！"}, {U"?", U"？"}, {U"~", U"〜"},
    };
    return table;
}

// 全キーの真の接頭辞集合（次の入力を待つべきか判定に使う）。
// 「n」+子音 → 「ん」を機能させるため "n" 自体は完全キーにしない代わりに
// 接頭辞として待つ（na, ni, ... nn, n' の接頭辞なので自動的に含まれる）
const std::unordered_set<std::u32string>& Prefixes() {
    static const std::unordered_set<std::u32string> prefixes = [] {
        std::unordered_set<std::u32string> set;
        for (const auto& [key, value] : DefaultTable()) {
            for (size_t len = key.size() - 1; len >= 1; --len) {
                set.insert(key.substr(0, len));
            }
        }
        return set;
    }();
    return prefixes;
}

bool IsSokuonConsonant(char32_t c) {
    // 促音になる子音（nは含まない。nnは変換テーブル側で「ん」になる）
    static const std::u32string consonants = U"bcdfghjklmpqrstvwxyz";
    return consonants.find(c) != std::u32string::npos;
}

char32_t ToLowerAscii(char32_t c) {
    return (c >= U'A' && c <= U'Z') ? c + (U'a' - U'A') : c;
}

} // namespace

RomajiComposer::RomajiComposer(std::u32string comma, std::u32string period)
    : table_(DefaultTable()) {
    table_[U","] = std::move(comma);
    table_[U"."] = std::move(period);
}

void RomajiComposer::Input(char32_t character) {
    raw_.push_back(character);
    // 英字は小文字に正規化して扱う
    pending_.push_back(ToLowerAscii(character));
    Resolve();
}

void RomajiComposer::Input(const std::u32string& text) {
    for (char32_t c : text) Input(c);
}

void RomajiComposer::DeleteBackward() {
    if (!pending_.empty()) {
        pending_.pop_back();
        // 未解決ローマ字の削除はrawと1対1で対応する
        if (!raw_.empty()) raw_.pop_back();
    } else if (!text_.empty()) {
        text_.pop_back();
        // かな1文字は複数打鍵に対応するためrawとの対応が崩れる
        rawIsReliable_ = false;
    }
}

void RomajiComposer::Flush() {
    while (!pending_.empty()) {
        auto it = table_.find(pending_);
        if (it != table_.end()) {
            text_ += it->second;
            pending_.clear();
        } else {
            // 先頭から可能な限り変換し、残りは文字として確定
            const std::u32string before = pending_;
            ForceResolveHead();
            if (pending_ == before) {
                text_.push_back(pending_.front());
                pending_.erase(0, 1);
            }
        }
    }
}

void RomajiComposer::Clear() {
    text_.clear();
    pending_.clear();
    raw_.clear();
    rawIsReliable_ = true;
}

void RomajiComposer::Resolve() {
    while (!pending_.empty()) {
        const bool canExtend = Prefixes().count(pending_) > 0;
        auto it = table_.find(pending_);
        if (it != table_.end() && !canExtend) {
            text_ += it->second;
            pending_.clear();
            continue;
        }
        if (canExtend) {
            return; // 次の入力を待つ（例: "k", "n", "ch"）
        }
        ForceResolveHead();
    }
}

void RomajiComposer::ForceResolveHead() {
    // 最長一致する完全なキーを探す（例: "nk" → "n"=ん を出して "k" を残す）
    for (size_t len = pending_.size() - 1; len >= 1; --len) {
        auto it = table_.find(pending_.substr(0, len));
        if (it != table_.end()) {
            text_ += it->second;
            pending_.erase(0, len);
            return;
        }
    }
    // 促音: 同一子音の連続、または tch
    if (pending_.size() >= 2) {
        const char32_t c0 = pending_[0];
        const char32_t c1 = pending_[1];
        if ((c0 == c1 && IsSokuonConsonant(c0)) || (c0 == U't' && c1 == U'c')) {
            text_ += U"っ";
            pending_.erase(0, 1);
            return;
        }
    }
    // 「n」+子音（および単独の n の確定）→「ん」
    if (!pending_.empty() && pending_.front() == U'n') {
        text_ += U"ん";
        pending_.erase(0, 1);
        return;
    }
    // 変換不能な文字はそのまま出力
    text_.push_back(pending_.front());
    pending_.erase(0, 1);
}

} // namespace iroha
