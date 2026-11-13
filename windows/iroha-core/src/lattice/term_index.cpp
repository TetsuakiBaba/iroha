#include "lattice/term_index.h"

#include <algorithm>
#include <unordered_set>

#include "iroha/unicode.h"
#include "iroha/user_dictionary.h"
#include "lattice/dicdata_store.h"

namespace iroha {

namespace {

int CidFor(TermKind kind) {
    switch (kind) {
    case TermKind::kPerson: return 1289;     // 人名一般
    case TermKind::kPlace: return 1293;      // 地名一般
    case TermKind::kProperNoun: return lattice::cid::kProperNoun;
    case TermKind::kNoun: break;
    }
    return lattice::cid::kCommonNoun;
}

} // namespace

TermLatticeIndex::TermLatticeIndex(const std::unordered_map<char32_t, uint8_t>& charIds,
                                   const std::vector<TermDictionaryPack>& packs) {
    nodes_.emplace_back();
    std::unordered_set<std::u32string> seen;
    std::vector<uint8_t> ids;
    for (const TermDictionaryPack& pack : packs) {
        const int cid = CidFor(pack.kind);
        for (const TermPair& pair : pack.pairs) {
            const std::u32string reading = UserDictionary::NormalizedReading(pair.reading);
            const std::vector<std::u32string> characters = SplitGraphemes(reading);
            if (characters.size() < TermDictionary::kMinimumReadingLength || pair.word.empty() ||
                !seen.insert(reading + U"\t" + pair.word).second) {
                continue;
            }
            // 読みはカタカナにしてから文字 ID にする（本体の辞書と同じ）
            const std::u32string ruby = lattice::ToKatakana(reading);
            ids.clear();
            bool known = true;
            for (const std::u32string& character : SplitGraphemes(ruby)) {
                const auto it = character.size() == 1 ? charIds.find(character[0]) : charIds.end();
                if (it == charIds.end()) {
                    known = false;
                    break;
                }
                ids.push_back(it->second);
            }
            if (!known) continue;
            int node = kRootNode;
            for (uint8_t id : ids) node = AddChild(node, id);
            nodes_[node].entries.emplace_back(pair.word, ruby, cid, lattice::mid::kGeneral,
                                              TermDictionary::kLatticeValue);
            ++entryCount_;
        }
    }
}

int TermLatticeIndex::AddChild(int node, uint8_t charId) {
    auto& children = nodes_[node].children;
    const auto it = std::lower_bound(
        children.begin(), children.end(), charId,
        [](const std::pair<uint8_t, int>& child, uint8_t id) { return child.first < id; });
    if (it != children.end() && it->first == charId) return it->second;
    const int child = static_cast<int>(nodes_.size());
    // 先に挿入してから nodes_ を伸ばす（伸ばすと children の参照が無効になる）
    children.insert(it, {charId, child});
    nodes_.emplace_back();
    return child;
}

std::optional<int> TermLatticeIndex::Child(int node, uint8_t charId) const {
    if (node < 0 || static_cast<size_t>(node) >= nodes_.size()) return std::nullopt;
    const auto& children = nodes_[node].children;
    const auto it = std::lower_bound(
        children.begin(), children.end(), charId,
        [](const std::pair<uint8_t, int>& child, uint8_t id) { return child.first < id; });
    if (it == children.end() || it->first != charId) return std::nullopt;
    return it->second;
}

const std::vector<lattice::DicdataElement>& TermLatticeIndex::Entries(int node) const {
    static const std::vector<lattice::DicdataElement> kEmpty;
    if (node < 0 || static_cast<size_t>(node) >= nodes_.size()) return kEmpty;
    return nodes_[node].entries;
}

} // namespace iroha
