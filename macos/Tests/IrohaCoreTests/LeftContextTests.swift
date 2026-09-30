import XCTest
@testable import IrohaCore

final class LeftContextTests: XCTestCase {
    func testKeepsShortTextAsIs() {
        XCTAssertEqual(LeftContext.normalize("天気予報によると"), "天気予報によると")
        XCTAssertEqual(LeftContext.normalize(""), "")
    }

    func testKeepsOnlyCurrentLine() {
        XCTAssertEqual(LeftContext.normalize("前の段落。\n次の段落の"), "次の段落の")
        XCTAssertEqual(LeftContext.normalize("a\r\nb"), "b")
        XCTAssertEqual(LeftContext.normalize("悟りを開きました。\n神と化しています\n風呂上がりで"), "風呂上がりで")
    }

    func testEmptyWhenCursorIsAtLineStart() {
        XCTAssertEqual(LeftContext.normalize("前の行。\n"), "")
        XCTAssertEqual(LeftContext.normalize("\n"), "")
    }

    func testDropsLeadingWhitespaceOfLine() {
        XCTAssertEqual(LeftContext.normalize("前の行\n　  字下げした行の"), "字下げした行の")
        // 行の途中の空白は残す
        XCTAssertEqual(LeftContext.normalize("iroha, azookeyは"), "iroha, azookeyは")
    }

    func testTrimsToTrailingMaxLengthCharacters() {
        let text = String(repeating: "あ", count: 50) + "終"
        let result = LeftContext.normalize(text)
        XCTAssertEqual(result.count, LeftContext.maxLength)
        XCTAssertTrue(result.hasSuffix("終"))
    }
}
