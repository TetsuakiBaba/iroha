import XCTest
@testable import IrohaCore

final class EnglishInputDetectorTests: XCTestCase {

    private static let words: Set<String> = ["computer", "test", "make", "kore", "e-mail", "don't"]

    private func detect(_ raw: String) -> EnglishInputDetector.Outcome {
        EnglishInputDetector.detect(raw: raw, isEnglishWord: { Self.words.contains($0) })
    }

    func testLeftoverLettersAndEnglishWordComeNearTheTop() {
        // 「こmぷてr」「てst」: かなにならない英字が残り、英単語としても通る
        XCTAssertEqual(detect("computer"), .candidate(.init(word: "computer", placement: .near)))
        XCTAssertEqual(detect("test"), .candidate(.init(word: "test", placement: .near)))
        XCTAssertEqual(detect("don't"), .candidate(.init(word: "don't", placement: .near)))
    }

    func testRomajiThatIsAlsoAnEnglishWordGoesLate() {
        // 「まけ」「これ」: ローマ字として読めるので、英単語でも後ろに置く
        XCTAssertEqual(detect("make"), .candidate(.init(word: "make", placement: .late)))
        XCTAssertEqual(detect("kore"), .candidate(.init(word: "kore", placement: .late)))
    }

    func testLeftoverLettersAloneGoLate() {
        // 打ち間違いでも英字が残る（「とうきょうtぽ」）。打鍵どおりの英字は後ろに置く
        XCTAssertEqual(detect("toukyoutpo"), .candidate(.init(word: "toukyoutpo", placement: .late)))
    }

    func testPlainRomajiGetsNoCandidate() {
        XCTAssertEqual(detect("kyouha"), .rejected(.romaji))
        XCTAssertEqual(detect("kan"), .rejected(.romaji))
    }

    func testPunctuationAndShortInputGetNoCandidate() {
        XCTAssertEqual(detect("test."), .rejected(.notWordShaped))
        XCTAssertEqual(detect("t"), .rejected(.notWordShaped))
        XCTAssertEqual(detect("123"), .rejected(.notWordShaped))
    }

    /// 打ち間違いの訂正で読みを丸ごと置き換えても、打鍵は残る（英字の候補はこれを使う）
    func testRawSurvivesTypoCorrection() {
        var composer = RomajiComposer()
        composer.input("computer")
        composer.flush()
        composer.replaceText("こもうてる")
        XCTAssertEqual(composer.raw, "computer")
        XCTAssertTrue(composer.rawCoversInput)
        // F9/F10 は読みと打鍵の対応が要るので使わせない
        XCTAssertFalse(composer.rawIsReliable)
    }

    func testDeletingKanaLosesRaw() {
        var composer = RomajiComposer()
        composer.input("test")
        composer.flush()
        composer.deleteBackward()
        composer.deleteBackward()
        composer.deleteBackward()
        XCTAssertFalse(composer.rawCoversInput)
    }
}
