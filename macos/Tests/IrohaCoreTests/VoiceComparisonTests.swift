import XCTest
@testable import IrohaCore

final class VoiceComparisonTests: XCTestCase {

    private func difference(_ recognized: String, _ converted: String) -> VoiceComparison.Difference {
        VoiceComparison.Difference(recognized: recognized, converted: converted)
    }

    func testSameTextHasNoDifference() {
        XCTAssertEqual(VoiceComparison.differences(recognized: "今日は良い天気です", converted: "今日は良い天気です"), [])
        XCTAssertEqual(VoiceComparison.summary(recognized: "今日は", converted: "今日は"), "音声認識と同じ")
    }

    func testOneDifferenceWithContext() {
        XCTAssertEqual(
            VoiceComparison.differences(recognized: "明日の会議は何時からですか", converted: "明日の会議はいつからですか"),
            [difference("は何時か", "はいつか")])
    }

    func testDifferenceAtStartAndEnd() {
        XCTAssertEqual(
            VoiceComparison.differences(recognized: "立石さんと話す", converted: "停止さんと話す"),
            [difference("立石さ", "停止さ")])
        XCTAssertEqual(
            VoiceComparison.differences(recognized: "それは機関", converted: "それは期間"),
            [difference("は機関", "は期間")])
    }

    func testSeparateDifferences() {
        XCTAssertEqual(
            VoiceComparison.differences(
                recognized: "立石さんと明日の会議で記譜について話す", converted: "停止さんと明日の会議で寄付について話す"),
            [difference("立石さ", "停止さ"), difference("で記譜に", "で寄付に")])
    }

    func testDifferencesOneCharacterApartAreMerged() {
        // 「行」と「た」の間の同じ文字は「っ」の 1 文字だけ
        XCTAssertEqual(
            VoiceComparison.differences(recognized: "そこに行ったら", converted: "そこに言ってら"),
            [difference("に行ったら", "に言ってら")])
    }

    func testDifferentLengths() {
        XCTAssertEqual(
            VoiceComparison.differences(recognized: "良い天気", converted: "よい天気"),
            [difference("良い", "よい")])
        XCTAssertEqual(
            VoiceComparison.differences(recognized: "今日は", converted: "今日は。"),
            [difference("は", "は。")])
    }

    func testSummaryLimitsCount() {
        let summary = VoiceComparison.summary(
            recognized: "立石さんと明日の会議で記譜について話す", converted: "停止さんと明日の会議で寄付について話す", limit: 1)
        XCTAssertEqual(summary, "音声認識 → iroha: 立石さ → 停止さ　ほか 1 か所")
    }
}
