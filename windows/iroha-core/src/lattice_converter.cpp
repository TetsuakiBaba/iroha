#include "iroha/lattice_converter.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_set>

#include "iroha/unicode.h"
#include "lattice/dicdata_store.h"
#include "lattice/kana_kanji_converter.h"
#include "lattice/term_index.h"

namespace iroha {

struct LatticeConverter::Impl {
    explicit Impl(const std::filesystem::path& dictionaryPath)
        : store(dictionaryPath), converter(&store) {}

    // `useTerms` が真なら専門辞書も引く（候補を作るとき）
    std::vector<lattice::ConvertedCandidate> Request(const std::u32string& reading, int count,
                                                     bool useTerms) {
        store.SetTermIndex(useTerms ? termIndex.get() : nullptr);
        // macOS 版と同じく、探索で残す数は 5 以上にする
        std::vector<lattice::ConvertedCandidate> result =
            converter.RequestCandidates(reading, std::max(count, 5), true);
        store.SetTermIndex(nullptr);
        return result;
    }

    lattice::DicdataStore store;
    lattice::KanaKanjiConverter converter;
    std::shared_ptr<const TermLatticeIndex> termIndex;
};

std::optional<std::filesystem::path> LatticeConverter::DefaultDictionaryPath(
    const std::filesystem::path& executableDirectory) {
    std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
    wchar_t* env = nullptr;
    size_t envLength = 0;
    if (_wdupenv_s(&env, &envLength, L"IROHA_DICTIONARY") == 0 && env != nullptr) {
        if (env[0] != L'\0') candidates.emplace_back(env);
        std::free(env);
    }
#else
    if (const char* env = std::getenv("IROHA_DICTIONARY")) {
        if (env[0] != '\0') candidates.emplace_back(env);
    }
#endif
    if (!executableDirectory.empty()) candidates.push_back(executableDirectory / "Dictionary");
    std::error_code error;
    std::filesystem::path directory = std::filesystem::current_path(error);
    if (!error) {
        for (int i = 0; i < 4; ++i) {
            candidates.push_back(directory / "vendor" / "azooKey_dictionary_storage" / "Dictionary");
            directory = directory.parent_path();
        }
    }
    for (const std::filesystem::path& candidate : candidates) {
        if (IsDictionaryDirectory(candidate)) return candidate;
    }
    return std::nullopt;
}

bool LatticeConverter::IsDictionaryDirectory(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path / "mm.binary", error) &&
           std::filesystem::exists(path / "louds" / "charID.chid", error);
}

LatticeConverter::LatticeConverter(const std::filesystem::path& dictionaryPath)
    : impl_(std::make_unique<Impl>(dictionaryPath)) {}

LatticeConverter::~LatticeConverter() = default;

std::vector<std::u32string> LatticeConverter::Candidates(const std::u32string& reading, int count) {
    std::vector<std::u32string> results = FullMatchCandidates(reading, count);
    if (results.size() > static_cast<size_t>(std::max(count, 0))) {
        results.resize(static_cast<size_t>(std::max(count, 0)));
    }
    return results;
}

std::vector<std::u32string> LatticeConverter::FullMatchCandidates(const std::u32string& reading,
                                                                  int nBest) {
    std::unordered_set<std::u32string> seen;
    std::vector<std::u32string> results;
    for (const Candidate& candidate : RawCandidates(reading, nBest)) {
        if (!candidate.isFullMatch || !seen.insert(candidate.text).second) continue;
        results.push_back(candidate.text);
        if (results.size() >= static_cast<size_t>(kMaxFullMatchCandidates)) break;
    }
    return results;
}

std::vector<LatticeConverter::Candidate> LatticeConverter::RawCandidates(
    const std::u32string& reading, int count) {
    std::vector<Candidate> results;
    if (reading.empty()) return results;
    const int readingLength = static_cast<int>(GraphemeCount(reading));
    for (const lattice::ConvertedCandidate& candidate : impl_->Request(reading, count, true)) {
        results.push_back({candidate.text, static_cast<double>(candidate.value),
                           candidate.rubyCount == readingLength});
    }
    return results;
}

std::optional<std::set<int>> LatticeConverter::WordBoundaries(
    const std::u32string& reading,
    const std::vector<std::pair<std::u32string, std::u32string>>& userWords) {
    if (reading.empty()) return std::nullopt;
    std::vector<lattice::DicdataElement> words;
    words.reserve(userWords.size());
    for (const auto& [wordReading, word] : userWords) {
        words.emplace_back(word, lattice::ToKatakana(wordReading), lattice::cid::kProperNoun,
                           lattice::mid::kGeneral, lattice::PValue(-10));
    }
    impl_->store.SetDynamicUserDictionary(std::move(words));
    // 専門辞書の語は入れない
    const std::vector<lattice::ConvertedCandidate> candidates = impl_->Request(reading, 1, false);
    impl_->store.SetDynamicUserDictionary({});

    const int readingLength = static_cast<int>(GraphemeCount(reading));
    for (const lattice::ConvertedCandidate& candidate : candidates) {
        if (candidate.rubyCount != readingLength) continue;
        std::set<int> boundaries = {0};
        int position = 0;
        for (const lattice::DicdataElement& element : candidate.data) {
            position += static_cast<int>(GraphemeCount(element.ruby));
            boundaries.insert(position);
        }
        return boundaries;
    }
    return std::nullopt;
}

std::shared_ptr<const TermLatticeIndex> LatticeConverter::BuildTermIndex(
    const std::filesystem::path& dictionaryPath, const std::vector<TermDictionaryPack>& packs) {
    return std::make_shared<const TermLatticeIndex>(lattice::DicdataStore::LoadCharIds(dictionaryPath),
                                                    packs);
}

size_t LatticeConverter::TermIndexEntryCount(const TermLatticeIndex& index) {
    return index.EntryCount();
}

void LatticeConverter::SetTermDictionary(std::shared_ptr<const TermLatticeIndex> index) {
    impl_->termIndex = std::move(index);
}

} // namespace iroha
