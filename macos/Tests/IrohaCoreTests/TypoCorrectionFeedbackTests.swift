import XCTest
@testable import IrohaCore

/// 「何をどう直したか」を小窓に出すための割り付け（モデルは読まないので常に走る）
final class TypoCorrectionFeedbackTests: XCTestCase {

    private func feedback(
        _ reading: String, _ corrected: String, context: Int = 6, maxChanged: Int = 12
    ) -> TypoCorrectionFeedback? {
        TypoCorrectionFeedback.make(
            from: TypoCorrection(reading: reading, corrected: corrected, margin: 5),
            context: context, maxChanged: maxChanged)
    }

    /// 置換: 変わった1文字だけが左右で入れ替わり、前後は共通
    func testSubstitution() {
        let result = feedback("をわぇてけいやくする", "をわけてけいやくする")
        XCTAssertEqual(result?.prefix, "をわ")
        XCTAssertEqual(result?.originalChanged, "ぇ")
        XCTAssertEqual(result?.correctedChanged, "け")
        XCTAssertEqual(result?.suffix, "てけいやくす…")
    }

    /// 削除: 訂正後の変わった部分が空になる（左の取り消し線で「消えた」が読める）
    func testDeletion() {
        let result = feedback("がちがうんっだろな", "がちがうんだろな")
        XCTAssertEqual(result?.originalChanged, "っ")
        XCTAssertEqual(result?.correctedChanged, "")
        XCTAssertEqual(result?.original, "がちがうんっだろな")
        XCTAssertEqual(result?.corrected, "がちがうんだろな")
    }

    /// 挿入: 打った読みの変わった部分が空になる
    func testInsertion() {
        let result = feedback("とうきょうえき", "とうきょうのえき")
        XCTAssertEqual(result?.originalChanged, "")
        XCTAssertEqual(result?.correctedChanged, "の")
        XCTAssertEqual(result?.corrected, "とうきょうのえき")
    }

    /// 差分が無ければ何も出さない（空の小窓を出さないための保証）
    func testNoDifferenceReturnsNil() {
        XCTAssertNil(feedback("こんにちは", "こんにちは"))
    }

    /// 前後は元と訂正後で必ず同じ文字列。片方だけ削ると2つを並べたとき桁がずれる
    func testTrimsContextSymmetrically() {
        let reading = String(repeating: "あ", count: 20) + "ぇ" + String(repeating: "い", count: 20)
        let corrected = String(repeating: "あ", count: 20) + "け" + String(repeating: "い", count: 20)
        let result = feedback(reading, corrected)
        XCTAssertEqual(result?.prefix, "…ああああああ")
        XCTAssertEqual(result?.suffix, "いいいいいい…")
        XCTAssertEqual(result?.original, "…ああああああぇいいいいいい…")
        XCTAssertEqual(result?.corrected, "…ああああああけいいいいいい…")
    }

    /// 差分が読みの先頭・末尾にあるときは、その側に省略記号を付けない
    func testDiffAtHeadAndTail() {
        let head = feedback("をんにちは", "こんにちは")
        XCTAssertEqual(head?.prefix, "")
        XCTAssertEqual(head?.suffix, "んにちは")

        let tail = feedback("こんにちわ", "こんにちは")
        XCTAssertEqual(tail?.prefix, "こんにち")
        XCTAssertEqual(tail?.suffix, "")
    }

    /// 変わった部分が長くても小窓が伸び続けないよう切る。切ったら "…" を付ける
    func testTruncatesLongChangedPart() {
        let result = feedback(
            String(repeating: "さ", count: 20), String(repeating: "し", count: 20), maxChanged: 4)
        XCTAssertEqual(result?.originalChanged, "ささささ…")
        XCTAssertEqual(result?.correctedChanged, "しししし…")
    }
}
