#pragma once
#include <cstddef>
#include <optional>

namespace iroha {

// 候補ウィンドウの並びと選択の動き（表示は持たない）。移植元: macos/Sources/IrohaCore/CandidateGrid.swift。
//
// 候補は 9 件ずつの列に分ける（列の中の番号 1〜9 で選べる）。最初は今の候補がある 1 列だけを見せ、
// Tab で列を横に並べた表に広げる。表に広げている間だけ ←→ で隣の列へ移る
// （閉じているときの ←→ は文節の移動なので、ここでは扱わない）。
// 候補の並びは列優先なので、↓・Space は表に広げていても 1 列のときと同じく「次の候補」へ進む
class CandidateGrid {
public:
    // 1 列に並べる件数（番号キー 1〜9 に合わせる）
    static constexpr size_t kRowsPerColumn = 9;

    explicit CandidateGrid(size_t count, size_t selected = 0, bool expanded = false)
        : count_(count), selected_(count > 0 ? (selected < count ? selected : count - 1) : 0),
          expanded_(expanded && count > kRowsPerColumn) {}

    size_t Count() const { return count_; }
    size_t Selected() const { return selected_; }
    bool IsExpanded() const { return expanded_; }
    size_t ColumnCount() const { return (count_ + kRowsPerColumn - 1) / kRowsPerColumn; }
    size_t SelectedColumn() const { return selected_ / kRowsPerColumn; }
    size_t SelectedRow() const { return selected_ % kRowsPerColumn; }
    // 1 列に収まらないときだけ広げる意味がある
    bool CanExpand() const { return ColumnCount() > 1; }

    // 次の候補へ（最後の次は先頭に戻る）
    void Next() {
        if (count_ > 0) selected_ = (selected_ + 1) % count_;
    }
    // 前の候補へ（先頭の前は最後に戻る）
    void Previous() {
        if (count_ > 0) selected_ = (selected_ + count_ - 1) % count_;
    }

    // 表に広げているときに隣の列の同じ行へ移る。移る先の列が短ければその列の最後の候補。
    // 端の列からさらに外へは移らない。動いたら真
    bool MoveColumn(int delta) {
        const long column = static_cast<long>(SelectedColumn()) + delta;
        if (!expanded_ || delta == 0 || column < 0 || column >= static_cast<long>(ColumnCount())) return false;
        const size_t start = static_cast<size_t>(column) * kRowsPerColumn;
        const size_t end = start + kRowsPerColumn < count_ ? start + kRowsPerColumn : count_;
        const size_t target = start + SelectedRow();
        selected_ = target < end ? target : end - 1;
        return true;
    }

    // 番号キー（1〜9）が指す候補。今の候補がある列の中で数える。その番号の候補が無ければ無し
    std::optional<size_t> IndexForNumber(size_t number) const {
        if (number < 1 || number > kRowsPerColumn) return std::nullopt;
        const size_t index = SelectedColumn() * kRowsPerColumn + number - 1;
        if (index >= count_) return std::nullopt;
        return index;
    }

    // 表に広げる・1 列に戻す。1 列に収まる候補では何もしない。変わったら真
    bool ToggleExpanded() {
        if (!CanExpand() && !expanded_) return false;
        expanded_ = !expanded_;
        return true;
    }

private:
    size_t count_;
    size_t selected_;
    bool expanded_ = false;
};

} // namespace iroha
