import XCTest
@testable import IrohaCore

final class KanaTypeKeysTests: XCTestCase {
    func testWindowsStyle() {
        let style = KanaTypeKeyStyle.windows
        XCTAssertEqual(style.conversion(forControlKey: "u"), .hiragana)
        XCTAssertEqual(style.conversion(forControlKey: "i"), .katakana)
        XCTAssertEqual(style.conversion(forControlKey: "o"), .halfWidthKatakana)
        XCTAssertEqual(style.conversion(forControlKey: "p"), .fullWidthAlphanumeric)
        XCTAssertEqual(style.conversion(forControlKey: "t"), .halfWidthAlphanumeric)
        XCTAssertNil(style.conversion(forControlKey: "k"))
    }

    func testKotoeriStyle() {
        let style = KanaTypeKeyStyle.kotoeri
        XCTAssertEqual(style.conversion(forControlKey: "j"), .hiragana)
        XCTAssertEqual(style.conversion(forControlKey: "k"), .katakana)
        XCTAssertEqual(style.conversion(forControlKey: ";"), .halfWidthKatakana)
        XCTAssertEqual(style.conversion(forControlKey: "l"), .fullWidthAlphanumeric)
        XCTAssertEqual(style.conversion(forControlKey: ":"), .halfWidthAlphanumeric)
        XCTAssertEqual(style.conversion(forControlKey: "'"), .halfWidthAlphanumeric)
        XCTAssertNil(style.conversion(forControlKey: "u"))
        XCTAssertNil(style.conversion(forControlKey: "i"))
    }

    func testAtokStyle() {
        let style = KanaTypeKeyStyle.atok
        XCTAssertEqual(style.conversion(forControlKey: "u"), .hiragana)
        XCTAssertEqual(style.conversion(forControlKey: "@"), .halfWidthAlphanumeric)
        XCTAssertNil(style.conversion(forControlKey: "t"))
    }

    /// どの系列も 5 種類の変換をすべて持つ
    func testEveryStyleCoversAllConversions() {
        let keys = ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r",
                    "s", "t", "u", "v", "w", "x", "y", "z", ";", ":", "'", "@"]
        for style in KanaTypeKeyStyle.allCases {
            let covered = Set(keys.compactMap { style.conversion(forControlKey: $0).map { "\($0)" } })
            XCTAssertEqual(covered.count, KanaTypeConversion.allCases.count, "\(style)")
        }
    }
}
