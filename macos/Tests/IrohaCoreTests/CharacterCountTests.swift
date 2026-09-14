import XCTest
@testable import IrohaCore

final class CharacterCountTests: XCTestCase {
    func testCountsGraphemeClusters() {
        let count = CharacterCount(of: "こんにちは👨‍👩‍👧")
        XCTAssertEqual(count.total, 6)
        XCTAssertEqual(count.withoutWhitespace, 6)
        XCTAssertEqual(count.summary, "6文字")
    }

    func testNewlinesAreNotCounted() {
        let count = CharacterCount(of: "あい\nうえ\r\nお")
        XCTAssertEqual(count.total, 5)
        XCTAssertEqual(count.withoutWhitespace, 5)
    }

    func testWhitespaceIsCountedInTotalOnly() {
        let count = CharacterCount(of: "Hello world　テスト\t!")
        XCTAssertEqual(count.total, 17)
        XCTAssertEqual(count.withoutWhitespace, 14)
        XCTAssertEqual(count.summary, "17文字（空白除く 14）")
    }

    func testEmpty() {
        let count = CharacterCount(of: "\n\n")
        XCTAssertEqual(count.total, 0)
        XCTAssertEqual(count.summary, "0文字")
    }
}
