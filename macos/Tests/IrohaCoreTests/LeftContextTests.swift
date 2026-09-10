import XCTest
@testable import IrohaCore

final class LeftContextTests: XCTestCase {
    func testKeepsShortTextAsIs() {
        XCTAssertEqual(LeftContext.normalize("天気予報によると"), "天気予報によると")
        XCTAssertEqual(LeftContext.normalize(""), "")
    }

    func testRemovesNewlinesAndJoinsParagraphs() {
        XCTAssertEqual(LeftContext.normalize("前の段落。\n次の段落の"), "前の段落。次の段落の")
        XCTAssertEqual(LeftContext.normalize("a\r\nb"), "ab")
    }

    func testTrimsToTrailingMaxLengthCharacters() {
        let text = String(repeating: "あ", count: 50) + "終"
        let result = LeftContext.normalize(text)
        XCTAssertEqual(result.count, LeftContext.maxLength)
        XCTAssertTrue(result.hasSuffix("終"))
    }
}
