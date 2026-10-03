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

    // MARK: - 読みの中の英単語を英字に書き換える

    private static let rewriteWords: Set<String> = [
        "computer", "test", "make", "word", "comp", "out", "apple", "hello", "notes", "email", "woo",
    ]

    private func rewrite(_ raw: String, excluding: Set<String> = []) -> EnglishInputDetector.Rewrite? {
        EnglishInputDetector.rewrite(raw: raw, excluding: excluding, isEnglishWord: { Self.rewriteWords.contains($0) })
    }

    func testRewriteWholeWord() {
        XCTAssertEqual(rewrite("computer"), .init(reading: "computer", words: ["computer"]))
    }

    func testRewriteWordInsideJapanese() {
        XCTAssertEqual(rewrite("kyouhacomputerwotukau"),
                       .init(reading: "きょうはcomputerをつかう", words: ["computer"]))
        XCTAssertEqual(rewrite("sonotestnokekka"), .init(reading: "そのtestのけっか", words: ["test"]))
    }

    func testRewriteSeveralWords() {
        XCTAssertEqual(rewrite("testtoword"), .init(reading: "testとword", words: ["test", "word"]))
    }

    func testRomajiReadableWordsAreNotRewritten() {
        // 「まけ」はローマ字として読めるので英字にしない
        XCTAssertNil(rewrite("makeru"))
        XCTAssertNil(rewrite("kyouhamake"))
    }

    func testTypoLeftoverWithoutEnglishWordIsNotRewritten() {
        // 「とうきょうtぽ」: 残る英字を含む英単語の区間がない（「out」は音節の途中で切れる）
        XCTAssertNil(rewrite("toukyoutpo"))
    }

    func testExcludedWordsAreNotRewritten() {
        XCTAssertNil(rewrite("computer", excluding: ["computer"]))
    }

    func testPrefixOfLongerWordIsRewrittenThenExtended() {
        // 打ちかけの「comp」で休止すると「comp」になり、続きを打った次の休止で「computer」になる
        XCTAssertEqual(rewrite("comp"), .init(reading: "comp", words: ["comp"]))
        XCTAssertEqual(rewrite("computer"), .init(reading: "computer", words: ["computer"]))
    }

    func testPrefersSpanCoveringMoreLeftoverLetters() {
        // 「そのてstの」: 「notes」は残る s だけ、「test」は s と t を含む
        XCTAssertEqual(rewrite("sonotestnokekka"), .init(reading: "そのtestのけっか", words: ["test"]))
    }

    func testLeftoverIsOnlyTheLetterThatStays() {
        // 「lw」は「lwa（ゎ）」の打ちかけなので l は o が来た時点で解決されるが、残るのは l だけ（「woo」は英字にしない）
        XCTAssertEqual(rewrite("emailwookuru"), .init(reading: "emailをおくる", words: ["email"]))
    }
}
