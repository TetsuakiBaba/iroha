// 移植元は louds.h のコメントを参照
#include "lattice/louds.h"

#include <fstream>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace iroha::lattice {

namespace {

constexpr int kUnit = 64;
constexpr int kUnitExponent = 6;

// 上位から続く 0 のビット数（0 のときは 64）
int LeadingZeroBitCount(uint64_t value) {
    if (value == 0) return kUnit;
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanReverse64(&index, value);
    return kUnit - 1 - static_cast<int>(index);
#else
    return __builtin_clzll(value);
#endif
}

int NonzeroBitCount(uint64_t value) {
    int count = 0;
    for (; value != 0; value &= value - 1) ++count;
    return count;
}

// Swift の `<<` と同じく、幅以上のシフトは 0 にする
uint64_t ShiftLeft(uint64_t value, int amount) {
    return amount >= kUnit ? 0 : value << amount;
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>* bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamsize size = file.tellg();
    if (size < 0) return false;
    bytes->resize(static_cast<size_t>(size));
    file.seekg(0);
    if (size > 0) file.read(reinterpret_cast<char*>(bytes->data()), size);
    return static_cast<bool>(file);
}

} // namespace

std::optional<Louds> Louds::Load(const std::filesystem::path& loudsPath,
                                 const std::filesystem::path& charsPath) {
    std::vector<uint8_t> chars;
    if (!ReadFile(charsPath, &chars)) return std::nullopt;
    std::vector<uint8_t> raw;
    if (!ReadFile(loudsPath, &raw)) return std::nullopt;
    std::vector<uint64_t> bits(raw.size() / 8);
    for (size_t i = 0; i < bits.size(); ++i) {
        uint64_t value = 0;
        for (int b = 7; b >= 0; --b) value = (value << 8) | raw[i * 8 + b];
        bits[i] = value;
    }
    return Louds(std::move(bits), chars);
}

Louds::Louds(std::vector<uint64_t> bits, const std::vector<uint8_t>& nodeIndexToCharId)
    : bits_(std::move(bits)) {
    for (uint8_t id : nodeIndexToCharId) ++flatCharToNodeIndicesIndex_[id];
    for (int i = 1; i < 256; ++i) {
        flatCharToNodeIndicesIndex_[i] += flatCharToNodeIndicesIndex_[i - 1];
    }
    std::array<int, 256> counts{};
    flatCharToNodeIndices_.assign(nodeIndexToCharId.size(), 0);
    for (size_t i = 0; i < nodeIndexToCharId.size(); ++i) {
        const int id = nodeIndexToCharId[i];
        const int base = id == 0 ? 0 : flatCharToNodeIndicesIndex_[id - 1];
        flatCharToNodeIndices_[base + counts[id]] = static_cast<int>(i);
        ++counts[id];
    }

    rankLarge_.assign(bits_.size() + 1, 0);
    for (size_t i = 0; i < bits_.size(); ++i) {
        rankLarge_[i + 1] = rankLarge_[i] + static_cast<uint32_t>(kUnit - NonzeroBitCount(bits_[i]));
    }
}

std::pair<int, int> Louds::ChildNodeIndices(int parentNodeIndex) const {
    // 求めるのは、左側に parentNodeIndex 個の 0 がある最小の位置（start）と、
    // 左側に parentNodeIndex + 1 個の 0 がある最小の位置（end）
    int left = parentNodeIndex >> kUnitExponent;
    int right = static_cast<int>(rankLarge_.size()) - 1;
    while (left <= right) {
        const int mid = (left + right) / 2;
        if (static_cast<int>(rankLarge_[mid]) >= parentNodeIndex) {
            right = mid - 1;
        } else {
            left = mid + 1;
        }
    }
    if (left == static_cast<int>(rankLarge_.size()) || left == 0) return {0, 0};
    const int i = left - 1;
    const uint64_t word = bits_[i];
    int k = 0;
    for (int n = parentNodeIndex - static_cast<int>(rankLarge_[i]); n > 0; --n) {
        k = LeadingZeroBitCount(~ShiftLeft(word, k)) + k + 1;
    }
    const int start = (i << kUnitExponent) + k - parentNodeIndex + 1;
    if (static_cast<int>(rankLarge_[i + 1]) == parentNodeIndex) {
        // parentNodeIndex 個目の 0 が i 番目の末尾にある。次の 0 は後ろの UInt64 から探す
        size_t j = static_cast<size_t>(i) + 1;
        while (j < bits_.size() && bits_[j] == UINT64_MAX) ++j;
        if (j >= bits_.size()) return {start, start};
        const int a = LeadingZeroBitCount(~bits_[j]) % kUnit;
        return {start, (static_cast<int>(j) << kUnitExponent) + a - parentNodeIndex + 1};
    }
    const int a = (LeadingZeroBitCount(~ShiftLeft(word, k)) + k) % kUnit;
    return {start, (i << kUnitExponent) + a - parentNodeIndex + 1};
}

std::optional<int> Louds::SearchCharNodeIndex(int parentNodeIndex, uint8_t charId) const {
    const std::pair<int, int> children = ChildNodeIndices(parentNodeIndex);
    // 同じ文字 ID のノード番号は昇順に並んでいるので二分探索できる
    int left = charId == 0 ? 0 : flatCharToNodeIndicesIndex_[charId - 1];
    const int end = flatCharToNodeIndicesIndex_[charId];
    int right = end;
    while (left < right) {
        const int mid = (left + right) >> 1;
        if (children.first <= flatCharToNodeIndices_[mid]) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }
    if (left < end && flatCharToNodeIndices_[left] >= children.first &&
        flatCharToNodeIndices_[left] < children.second) {
        return flatCharToNodeIndices_[left];
    }
    return std::nullopt;
}

} // namespace iroha::lattice
