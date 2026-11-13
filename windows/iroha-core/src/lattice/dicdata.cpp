// 移植元は dicdata.h のコメントを参照
#include "lattice/dicdata.h"

#include <array>

namespace iroha::lattice {

namespace {

bool InRange(int value, int first, int last) { return first <= value && value <= last; }

int JudgeWordType(int id) {
    if (id == cid::kBOS || id == cid::kEOS) return 3;
    // 前置の機能語（接頭辞など）
    if (id == 1315 || id == 6 || InRange(id, 557, 560)) return 0;
    // 内容語
    if (InRange(id, 561, 867) || InRange(id, 1283, 1296) || InRange(id, 1306, 1309) ||
        InRange(id, 11, 52) || InRange(id, 555, 556) || InRange(id, 1281, 1282) ||
        id == 1314 || InRange(id, 1, 5) || id == 9) {
        return 1;
    }
    return 2;
}

// 元の実装は 0...1319 の 1320 個を持つ
constexpr int kWordTypeCount = 1320;

const std::array<unsigned char, kWordTypeCount>& WordTypes() {
    static const std::array<unsigned char, kWordTypeCount> table = [] {
        std::array<unsigned char, kWordTypeCount> values{};
        for (int id = 0; id < kWordTypeCount; ++id) {
            values[id] = static_cast<unsigned char>(JudgeWordType(id));
        }
        return values;
    }();
    return table;
}

} // namespace

int WordType(int id) {
    if (id < 0 || id >= kWordTypeCount) return 2;
    return WordTypes()[id];
}

bool IsClause(int formerRcid, int latterLcid) {
    const int latterType = WordType(latterLcid);
    if (latterType == 3) return false;
    const int formerType = WordType(formerRcid);
    if (formerType == 3) return false;
    if (latterType == 0 || latterType == 1) return formerType != 0;
    return false;
}

bool IncludeMMValueCalculation(const DicdataElement& data) {
    // 非自立動詞
    if (InRange(data.lcid, 895, 1280) || InRange(data.rcid, 895, 1280)) return true;
    // 非自立名詞
    if (InRange(data.lcid, 1297, 1305) || InRange(data.rcid, 1297, 1305)) return true;
    return WordType(data.lcid) == 1 || WordType(data.rcid) == 1;
}

} // namespace iroha::lattice
