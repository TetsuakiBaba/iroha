import XCTest
@testable import IrohaCore

final class CandidateGridTests: XCTestCase {
    func testNextAndPreviousWrapAround() {
        var grid = CandidateGrid(count: 3)
        grid.previous()
        XCTAssertEqual(grid.selected, 2)
        grid.next()
        XCTAssertEqual(grid.selected, 0)
    }

    func testNumbersCountWithinSelectedColumn() {
        var grid = CandidateGrid(count: 20)
        XCTAssertEqual(grid.index(forNumber: 1), 0)
        XCTAssertEqual(grid.index(forNumber: 9), 8)
        XCTAssertNil(grid.index(forNumber: 0))
        XCTAssertNil(grid.index(forNumber: 10))
        for _ in 0..<9 { grid.next() }  // 2 列目の先頭
        XCTAssertEqual(grid.selectedColumn, 1)
        XCTAssertEqual(grid.index(forNumber: 3), 11)
        for _ in 0..<9 { grid.next() }  // 3 列目（2 件だけ）
        XCTAssertEqual(grid.index(forNumber: 2), 19)
        XCTAssertNil(grid.index(forNumber: 3))
    }

    func testColumnsMoveOnlyWhenExpanded() {
        var grid = CandidateGrid(count: 20, selected: 4)
        XCTAssertFalse(grid.moveColumn(by: 1))
        XCTAssertEqual(grid.selected, 4)
        XCTAssertTrue(grid.toggleExpanded())
        XCTAssertTrue(grid.moveColumn(by: 1))
        XCTAssertEqual(grid.selected, 13)
        // 3 列目は 2 件しかないので、その列の最後に寄せる
        XCTAssertTrue(grid.moveColumn(by: 1))
        XCTAssertEqual(grid.selected, 19)
        // 端から外へは移らない
        XCTAssertFalse(grid.moveColumn(by: 1))
        XCTAssertTrue(grid.moveColumn(by: -2))
        XCTAssertEqual(grid.selected, 1)
        XCTAssertFalse(grid.moveColumn(by: -1))
    }

    func testSingleColumnDoesNotExpand() {
        var grid = CandidateGrid(count: 9)
        XCTAssertFalse(grid.canExpand)
        XCTAssertFalse(grid.toggleExpanded())
        XCTAssertFalse(grid.isExpanded)
    }

    func testEmptyGridIsHarmless() {
        var grid = CandidateGrid(count: 0)
        grid.next()
        grid.previous()
        XCTAssertEqual(grid.selected, 0)
        XCTAssertNil(grid.index(forNumber: 1))
        XCTAssertEqual(grid.columnCount, 0)
    }
}
