import XCTest
@testable import IrohaCore

final class EnglishInputDetectorTests: XCTestCase {

    private static let words: Set<String> = ["computer", "test", "make", "kore", "e-mail", "don't"]

    private func detect(_ raw: String) -> EnglishInputDetector.Candidate? {
        var composer = RomajiComposer()
        composer.input(raw)
        composer.flush()
        return EnglishInputDetector.candidate(
            raw: composer.raw, reading: composer.text, isEnglishWord: { Self.words.contains($0) })
    }

    func testLeftoverLettersAndEnglishWordComeNearTheTop() {
        // 「こmぷてr」「てst」: かなにならない英字が残り、英単語としても通る
        XCTAssertEqual(detect("computer"), .init(word: "computer", placement: .near))
        XCTAssertEqual(detect("test"), .init(word: "test", placement: .near))
        XCTAssertEqual(detect("don't"), .init(word: "don't", placement: .near))
    }

    func testRomajiThatIsAlsoAnEnglishWordGoesLate() {
        // 「まけ」「これ」: ローマ字として読めるので、英単語でも後ろに置く
        XCTAssertEqual(detect("make"), .init(word: "make", placement: .late))
        XCTAssertEqual(detect("kore"), .init(word: "kore", placement: .late))
    }

    func testLeftoverLettersAloneGoLate() {
        // 打ち間違いでも英字が残る（「とうきょうtぽ」）。打鍵どおりの英字は後ろに置く
        XCTAssertEqual(detect("toukyoutpo"), .init(word: "toukyoutpo", placement: .late))
    }

    func testPlainRomajiGetsNoCandidate() {
        XCTAssertNil(detect("kyouha"))
        XCTAssertNil(detect("kan"))
    }

    func testPunctuationAndShortInputGetNoCandidate() {
        XCTAssertNil(detect("test."))
        XCTAssertNil(detect("t"))
        XCTAssertNil(detect("123"))
    }
}
