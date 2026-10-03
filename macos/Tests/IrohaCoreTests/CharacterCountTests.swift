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

    func testEnglishShowsWords() {
        let count = CharacterCount(of: "The well-known model, e.g. GPT,\nwas released in 2026.")
        XCTAssertTrue(count.isEnglish)
        XCTAssertEqual(count.words, 9)
        XCTAssertEqual(count.total, 52)
        XCTAssertEqual(count.summary, "9 words (52 chars)")
    }

    func testSingularWord() {
        XCTAssertEqual(CharacterCount(of: "Hello").summary, "1 word (5 chars)")
    }

    func testAnyKanaOrKanjiMeansJapanese() {
        // 英語の文に日本語の語が1つでも混ざれば文字数の表示
        let count = CharacterCount(of: "I visited 東京 last year")
        XCTAssertFalse(count.isEnglish)
        XCTAssertEqual(count.summary, "22文字（空白除く 18）")
        XCTAssertFalse(CharacterCount(of: "ｱｲｳ abc").isEnglish)
    }

    func testDigitsOnlyIsNotEnglish() {
        XCTAssertEqual(CharacterCount(of: "2026 10 03").summary, "10文字（空白除く 8）")
    }

    func testEmpty() {
        let count = CharacterCount(of: "\n\n")
        XCTAssertEqual(count.total, 0)
        XCTAssertEqual(count.summary, "0文字")
    }
}
