import XCTest
@testable import IrohaCore

/// 予測として表示する範囲（先頭の1文節）の切り出し
final class PredictionTextTests: XCTestCase {

    private func phrase(_ text: String, maxLength: Int = 16) -> PredictionText.Phrase {
        PredictionText.phrase(in: text, maxLength: maxLength)
    }

    func testStopsAtPunctuationInclusive() {
        XCTAssertEqual(phrase("お願いします。よろしく"),
                       PredictionText.Phrase(text: "お願いします。", isComplete: true))
        XCTAssertEqual(phrase("そうですね、"),
                       PredictionText.Phrase(text: "そうですね、", isComplete: true))
    }

    /// かな → 非かな の位置を文節境界とみなし、1文節目で止める
    func testFirstBunsetsu() {
        XCTAssertEqual(phrase("いい天気ですね"), PredictionText.Phrase(text: "いい", isComplete: true))
        XCTAssertEqual(phrase("魚を食べる"), PredictionText.Phrase(text: "魚を", isComplete: true))
    }

    /// 1文節目が短い（助詞1文字など）ときは2文節目まで含める
    func testShortFirstBunsetsuExtendsToSecond() {
        XCTAssertEqual(phrase("は学生で"), PredictionText.Phrase(text: "は学生で", isComplete: false))
        XCTAssertEqual(phrase("は学生で大学に"), PredictionText.Phrase(text: "は学生で", isComplete: true))
        // 2文節目が終わる前に句読点が来たらそこまで
        XCTAssertEqual(phrase("は学生です。"), PredictionText.Phrase(text: "は学生です。", isComplete: true))
    }

    /// 境界がまだ出ていなければ続きを待つ
    func testIncompleteWithoutBoundary() {
        XCTAssertEqual(phrase("天気です"), PredictionText.Phrase(text: "天気です", isComplete: false))
        XCTAssertEqual(phrase(""), PredictionText.Phrase(text: "", isComplete: false))
    }

    func testNewlineWhitespaceAndSpecialTokensStop() {
        XCTAssertEqual(phrase("そうですね\nほげ"), PredictionText.Phrase(text: "そうですね", isComplete: true))
        XCTAssertEqual(phrase("そう ですね"), PredictionText.Phrase(text: "そう", isComplete: true))
        XCTAssertEqual(phrase("です\u{EE00}"), PredictionText.Phrase(text: "です", isComplete: true))
        XCTAssertEqual(phrase("\u{EE00}"), PredictionText.Phrase(text: "", isComplete: true))
        // 単独の異体字セレクタ（見えない書式文字）が出たらそこで止める。
        // 文字に結合したものは落として文字だけ残し、続きを待つ
        XCTAssertEqual(phrase("\u{FE0F}そうなんですね"), PredictionText.Phrase(text: "", isComplete: true))
        XCTAssertEqual(phrase("はい\u{FE0F}"), PredictionText.Phrase(text: "はい", isComplete: false))
        XCTAssertEqual(phrase("はい\u{FE0F}天気"), PredictionText.Phrase(text: "はい", isComplete: true))
    }

    func testMaxLengthCutsOff() {
        XCTAssertEqual(phrase("ですがそれではまたあとで", maxLength: 6),
                       PredictionText.Phrase(text: "ですがそれで", isComplete: true))
    }

    /// 長音符はカタカナ語の一部として扱い、そこで文節を切らない
    func testLongVowelMarkFollowsPrecedingCharacter() {
        XCTAssertEqual(phrase("コンピューターを使う"),
                       PredictionText.Phrase(text: "コンピューターを", isComplete: true))
    }
}
