import XCTest
@testable import IrohaCore

final class ReadingConstraintTests: XCTestCase {

    /// 出力が読みと辻褄が合うか（生成を最後まで走らせたときと同じ判定）
    private func accepts(_ reading: String, _ output: String) -> Bool {
        guard let constraint = ReadingConstraint(reading: reading) else { return true }
        let mask = constraint.advance(constraint.initialMask, text: output)
        return mask != 0 && constraint.isComplete(mask)
    }

    func testAcceptsPlainConversion() {
        XCTAssertTrue(accepts("こんにちはあかちゃん", "こんにちは赤ちゃん"))
        XCTAssertTrue(accepts("きょうはいいてんきですね", "今日はいい天気ですね"))
        XCTAssertTrue(accepts("うけたまわりました", "承りました"))
    }

    /// 読みにない句読点の挿入を弾く（zenzが「こんにちは。赤ちゃん」を出す問題）
    func testRejectsInsertedPunctuation() {
        XCTAssertFalse(accepts("こんにちはあかちゃん", "こんにちは。赤ちゃん"))
        XCTAssertTrue(accepts("こんにちは、あかちゃん", "こんにちは、赤ちゃん"))
    }

    func testRejectsMismatchedKana() {
        XCTAssertFalse(accepts("さかなをたべる", "魚が食べる"))
    }

    /// 読みを使い切らない・使い切りすぎる出力を弾く
    func testRejectsIncompleteConsumption() {
        XCTAssertFalse(accepts("あかちゃんがわらった", "赤ちゃんが"))
        XCTAssertFalse(accepts("あかちゃん", "赤ちゃんが笑った"))
    }

    /// カタカナ・英数字は読みと字数が合わないことがあるので許す
    func testAllowsLooseKatakanaAndAlphabet() {
        XCTAssertTrue(accepts("こんぴゅーた", "コンピューター"))
        XCTAssertTrue(accepts("わうわうをみる", "WOWOWを見る"))
        XCTAssertTrue(accepts("にせんにじゅうごねん", "2025年"))
    }

    /// 追跡できない読み（長すぎる・空）では制約をかけない
    func testNoConstraintForUntrackableReading() {
        XCTAssertNil(ReadingConstraint(reading: ""))
        XCTAssertNil(ReadingConstraint(reading: String(repeating: "あ", count: 63)))
        XCTAssertNotNil(ReadingConstraint(reading: String(repeating: "あ", count: 62)))
    }

    func testSpanClassification() {
        XCTAssertEqual(ReadingConstraint.span(of: "あ"), .literal)
        XCTAssertEqual(ReadingConstraint.span(of: "。"), .literal)
        XCTAssertEqual(ReadingConstraint.span(of: "!"), .literal)
        XCTAssertEqual(ReadingConstraint.span(of: "漢"), .oneOrMore)
        XCTAssertEqual(ReadingConstraint.span(of: "々"), .oneOrMore)
        XCTAssertEqual(ReadingConstraint.span(of: "ア"), .zeroOrMore)
        XCTAssertEqual(ReadingConstraint.span(of: "ー"), .zeroOrMore)
        XCTAssertEqual(ReadingConstraint.span(of: "7"), .zeroOrMore)
    }
}
