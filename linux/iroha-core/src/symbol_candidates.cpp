#include "iroha/symbol_candidates.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "iroha/unicode.h"
#include "iroha/user_dictionary.h"

namespace iroha {

namespace {

// 行に分け（空行と「#」で始まる行は除く）、行ごとにタブで区切る（空の列も残す）
std::vector<std::vector<std::u32string>> Rows(const std::string& utf8) {
    const std::u32string text = Utf8ToUtf32(utf8);
    std::vector<std::vector<std::u32string>> rows;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = start;
        while (end < text.size() && text[end] != U'\n' && text[end] != U'\r') ++end;
        if (end > start && text[start] != U'#') {
            std::vector<std::u32string> fields;
            size_t fieldStart = start;
            for (size_t i = start; i <= end; ++i) {
                if (i == end || text[i] == U'\t') {
                    fields.push_back(text.substr(fieldStart, i - fieldStart));
                    fieldStart = i + 1;
                }
            }
            rows.push_back(std::move(fields));
        }
        start = end + 1;
    }
    return rows;
}

// 空白で区切る（空の部分は捨てる）
std::vector<std::u32string> SplitBySpace(const std::u32string& text) {
    std::vector<std::u32string> parts;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(U' ', start);
        if (end == std::u32string::npos) end = text.size();
        if (end > start) parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

bool ReadFile(const std::filesystem::path& path, std::string* out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    *out = buffer.str();
    return true;
}

bool IsIdeographic(char32_t c) {
    return c == 0x3006 || c == 0x3007 || (c >= 0x3021 && c <= 0x3029) ||
           (c >= 0x3038 && c <= 0x303A) || (c >= 0x3400 && c <= 0x4DBF) ||
           (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0x20000 && c <= 0x3FFFF);
}

} // namespace

std::vector<std::u32string> SymbolCandidates::All() const {
    std::vector<std::u32string> all = emoji;
    all.insert(all.end(), emoticons.begin(), emoticons.end());
    all.insert(all.end(), symbols.begin(), symbols.end());
    return all;
}

const std::vector<std::string>& SymbolCandidateTable::FileNames() {
    static const std::vector<std::string> names = {"symbol.tsv", "emoticon.tsv", "categorized.tsv",
                                                   "emoji_data.tsv"};
    return names;
}

SymbolCandidateTable::SymbolCandidateTable(const std::string& symbolTsv,
                                           const std::string& emoticonTsv,
                                           const std::string& categorizedTsv,
                                           const std::string& emojiTsv,
                                           const RenderablePredicate& isRenderableEmoji) {
    // symbol.tsv: 品詞 / 文字 / 読み（空白区切り）/ 説明 / ...。1 行目は見出し
    for (const auto& fields : Rows(symbolTsv)) {
        if (fields.size() >= 3 && fields[0] != U"POS") Add(fields[1], fields[2], &symbols_, nullptr);
    }
    // emoticon.tsv: 顔文字 / 読み（空白区切り）/ 分類。1 行目は見出し（先頭の列が空）
    for (const auto& fields : Rows(emoticonTsv)) {
        if (fields.size() >= 2) Add(fields[0], fields[1], &emoticons_, &allEmoticons_);
    }
    // categorized.tsv: 顔文字 / 分類 / 読み（空白区切り）
    for (const auto& fields : Rows(categorizedTsv)) {
        if (fields.size() >= 3) Add(fields[0], fields[2], &emoticons_, &allEmoticons_);
    }
    // emoji_data.tsv: コードポイント / 絵文字 / 読み（空白区切り）/ ... / Emoji の版
    for (const auto& fields : Rows(emojiTsv)) {
        if (fields.size() < 3) continue;
        if (isRenderableEmoji && !isRenderableEmoji(fields[1])) continue;
        Add(fields[1], fields[2], &emoji_, &allEmoji_);
    }
    // 顔文字は 2 つのファイルの両方に載っているものがある
    std::unordered_set<std::u32string> seen;
    allEmoticons_.erase(std::remove_if(allEmoticons_.begin(), allEmoticons_.end(),
                                       [&seen](const std::u32string& value) {
                                           return !seen.insert(value).second;
                                       }),
                        allEmoticons_.end());
}

std::optional<SymbolCandidateTable> SymbolCandidateTable::Load(
    const std::filesystem::path& directory, const RenderablePredicate& isRenderableEmoji) {
    std::vector<std::string> texts;
    for (const std::string& name : FileNames()) {
        std::string text;
        if (!ReadFile(directory / name, &text)) return std::nullopt;
        texts.push_back(std::move(text));
    }
    return SymbolCandidateTable(texts[0], texts[1], texts[2], texts[3], isRenderableEmoji);
}

std::optional<std::filesystem::path> SymbolCandidateTable::DefaultDirectory(
    const std::filesystem::path& executableDirectory) {
    std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
    wchar_t* env = nullptr;
    size_t envLength = 0;
    if (_wdupenv_s(&env, &envLength, L"IROHA_SYMBOLS") == 0 && env != nullptr) {
        if (env[0] != L'\0') candidates.emplace_back(env);
        std::free(env);
    }
#else
    if (const char* env = std::getenv("IROHA_SYMBOLS")) {
        if (env[0] != '\0') candidates.emplace_back(env);
    }
#endif
    if (!executableDirectory.empty()) candidates.push_back(executableDirectory / "Symbols");
    std::error_code error;
    std::filesystem::path directory = std::filesystem::current_path(error);
    if (!error) {
        for (int i = 0; i < 4; ++i) {
            candidates.push_back(directory / "vendor" / "mozc-data");
            directory = directory.parent_path();
        }
    }
    for (const std::filesystem::path& candidate : candidates) {
        bool complete = true;
        for (const std::string& name : FileNames()) {
            if (!std::filesystem::is_regular_file(candidate / name, error)) complete = false;
        }
        if (complete) return candidate;
    }
    return std::nullopt;
}

SymbolCandidates SymbolCandidateTable::Candidates(const std::u32string& reading) const {
    const std::u32string key = UserDictionary::NormalizedReading(reading);
    const auto lookup = [&key](const Index& index) {
        const auto found = index.find(key);
        return found != index.end() ? found->second : std::vector<std::u32string>();
    };
    SymbolCandidates result;
    result.emoji = key == kAllEmojiReading ? allEmoji_ : lookup(emoji_);
    result.emoticons = key == kAllEmoticonsReading ? allEmoticons_ : lookup(emoticons_);
    result.symbols = lookup(symbols_);
    return result;
}

SymbolCandidateTable::Counts SymbolCandidateTable::GetCounts() const {
    std::unordered_set<std::u32string> symbols;
    for (const auto& item : symbols_) symbols.insert(item.second.begin(), item.second.end());
    return Counts{allEmoji_.size(), allEmoticons_.size(), symbols.size()};
}

// 読みのうち、ひらがなだけのもの（irohaの読みとして打てるもの）に value を登録する。
// Mozc の読みには「,」「:)」のような記号そのものも入っているが、irohaの読みはひらがななので使わない
void SymbolCandidateTable::Add(const std::u32string& value, const std::u32string& readings,
                               Index* table, std::vector<std::u32string>* all) {
    if (value.empty()) return;
    bool registered = false;
    for (const std::u32string& reading : SplitBySpace(readings)) {
        if (!UserDictionary::IsImportableReading(reading)) continue;
        std::vector<std::u32string>& values = (*table)[UserDictionary::NormalizedReading(reading)];
        if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
        registered = true;
    }
    if (registered && all) all->push_back(value);
}

// ---- 合流 ----

std::vector<std::u32string> SymbolCandidateMerger::Merge(
    const std::vector<std::u32string>& candidates, const SymbolCandidates& symbols,
    size_t protectedCount) {
    if (symbols.Empty()) return candidates;
    protectedCount = std::min(protectedCount, candidates.size());
    const std::unordered_set<std::u32string> emojiSet(symbols.emoji.begin(), symbols.emoji.end());
    std::vector<std::u32string> head(candidates.begin(), candidates.begin() + protectedCount);
    std::vector<std::u32string> tail;
    for (size_t i = protectedCount; i < candidates.size(); ++i) {
        if (emojiSet.count(candidates[i]) == 0) tail.push_back(candidates[i]);
    }

    std::unordered_set<std::u32string> seen(head.begin(), head.end());
    seen.insert(tail.begin(), tail.end());
    std::vector<std::u32string> emoji;
    for (const std::u32string& value : symbols.emoji) {
        if (seen.insert(value).second) emoji.push_back(value);
    }
    const size_t insertAt =
        std::min(std::max(protectedCount, kEmojiPosition), head.size() + tail.size()) - head.size();
    tail.insert(tail.begin() + static_cast<std::ptrdiff_t>(insertAt), emoji.begin(), emoji.end());
    for (const auto* list : {&symbols.emoticons, &symbols.symbols}) {
        for (const std::u32string& value : *list) {
            if (seen.insert(value).second) tail.push_back(value);
        }
    }
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
}

std::set<std::u32string> SymbolCandidateMerger::UnlearnableCandidates(
    const std::vector<std::u32string>& candidates, const SymbolCandidates& symbols) {
    std::set<std::u32string> result;
    const std::vector<std::u32string> all = symbols.All();
    const std::unordered_set<std::u32string> table(all.begin(), all.end());
    for (const std::u32string& candidate : candidates) {
        if (table.count(candidate) != 0 && !IsKanaOrKanji(candidate)) result.insert(candidate);
    }
    return result;
}

bool SymbolCandidateMerger::IsKanaOrKanji(const std::u32string& text) {
    if (text.empty()) return false;
    return std::all_of(text.begin(), text.end(), [](char32_t c) {
        return IsIdeographic(c) || (c >= 0x3041 && c <= 0x309F) || // ひらがな
               (c >= 0x30A0 && c <= 0x30FF) ||                     // カタカナ・長音符
               (c >= 0x31F0 && c <= 0x31FF) ||                     // カタカナの小書き
               (c >= 0x3005 && c <= 0x3006);                       // 々 〆
    });
}

} // namespace iroha
