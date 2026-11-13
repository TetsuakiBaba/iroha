#pragma once
// 候補ウィンドウの並びと選択の動きのテスト。
// 移植元: macos/Tests/IrohaCoreTests/CandidateGridTests.swift（1 対 1）。
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include "iroha/candidate_grid.h"

namespace {

void TestGridNextAndPreviousWrapAround() {
    iroha::CandidateGrid grid(3);
    grid.Previous();
    EXPECT_TRUE(grid.Selected() == 2);
    grid.Next();
    EXPECT_TRUE(grid.Selected() == 0);
}

void TestGridNumbersCountWithinSelectedColumn() {
    iroha::CandidateGrid grid(20);
    EXPECT_TRUE(grid.IndexForNumber(1) == std::optional<size_t>(0));
    EXPECT_TRUE(grid.IndexForNumber(9) == std::optional<size_t>(8));
    EXPECT_TRUE(!grid.IndexForNumber(0));
    EXPECT_TRUE(!grid.IndexForNumber(10));
    for (int i = 0; i < 9; ++i) grid.Next(); // 2 列目の先頭
    EXPECT_TRUE(grid.SelectedColumn() == 1);
    EXPECT_TRUE(grid.IndexForNumber(3) == std::optional<size_t>(11));
    for (int i = 0; i < 9; ++i) grid.Next(); // 3 列目（2 件だけ）
    EXPECT_TRUE(grid.IndexForNumber(2) == std::optional<size_t>(19));
    EXPECT_TRUE(!grid.IndexForNumber(3));
}

void TestGridColumnsMoveOnlyWhenExpanded() {
    iroha::CandidateGrid grid(20, 4);
    EXPECT_TRUE(!grid.MoveColumn(1));
    EXPECT_TRUE(grid.Selected() == 4);
    EXPECT_TRUE(grid.ToggleExpanded());
    EXPECT_TRUE(grid.MoveColumn(1));
    EXPECT_TRUE(grid.Selected() == 13);
    // 3 列目は 2 件しかないので、その列の最後に寄せる
    EXPECT_TRUE(grid.MoveColumn(1));
    EXPECT_TRUE(grid.Selected() == 19);
    // 端から外へは移らない
    EXPECT_TRUE(!grid.MoveColumn(1));
    EXPECT_TRUE(grid.MoveColumn(-2));
    EXPECT_TRUE(grid.Selected() == 1);
    EXPECT_TRUE(!grid.MoveColumn(-1));
}

void TestGridSingleColumnDoesNotExpand() {
    iroha::CandidateGrid grid(9);
    EXPECT_TRUE(!grid.CanExpand());
    EXPECT_TRUE(!grid.ToggleExpanded());
    EXPECT_TRUE(!grid.IsExpanded());
}

void TestGridEmptyIsHarmless() {
    iroha::CandidateGrid grid(0);
    grid.Next();
    grid.Previous();
    EXPECT_TRUE(grid.Selected() == 0);
    EXPECT_TRUE(!grid.IndexForNumber(1));
    EXPECT_TRUE(grid.ColumnCount() == 0);
}

void RunCandidateGridTests() {
    TestGridNextAndPreviousWrapAround();
    TestGridNumbersCountWithinSelectedColumn();
    TestGridColumnsMoveOnlyWhenExpanded();
    TestGridSingleColumnDoesNotExpand();
    TestGridEmptyIsHarmless();
}

} // namespace
