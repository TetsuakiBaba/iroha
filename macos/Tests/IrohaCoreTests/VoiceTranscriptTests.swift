import XCTest
@testable import IrohaCore

final class VoiceTranscriptTests: XCTestCase {

    private func token(_ surface: String, _ reading: String? = nil) -> ReadingToken {
        ReadingToken(surface: surface, reading: reading)
    }

    func testReadingsAreJoined() {
        let transcript = VoiceTranscript(tokens: [
            token("今日", "きょう"), token("は", "は"), token("良い", "よい"), token("天気", "てんき"),
            token("です", "です"), token("。", "。"),
        ])
        XCTAssertEqual(transcript.parts, [.reading("きょうはよいてんきです。", surface: "今日は良い天気です。")])
        XCTAssertEqual(transcript.surface, "今日は良い天気です。")
    }

    func testAlphabetAndDigitsStayFixed() {
        // SpeechTranscriber は英字・数字の前に空白を入れる
        let transcript = VoiceTranscript(tokens: [
            token("東京", "とうきょう"), token("で", "で"), token(" "), token("iPhone"), token("の", "の"),
            token("発表", "はっぴょう"),
        ])
        XCTAssertEqual(transcript.parts, [
            .reading("とうきょうで", surface: "東京で"),
            .fixed("iPhone"),
            .reading("のはっぴょう", surface: "の発表"),
        ])
        XCTAssertEqual(transcript.surface, "東京でiPhoneの発表")
        XCTAssertEqual(transcript.readingText, "とうきょうでiPhoneのはっぴょう")
    }

    func testSpaceBetweenFixedPartsIsKept() {
        let transcript = VoiceTranscript(tokens: [
            token("Apple"), token(" "), token("Watch"), token(" "), token("を", "を"),
        ])
        XCTAssertEqual(transcript.parts, [.fixed("Apple Watch"), .reading("を", surface: "を")])
    }

    func testLeadingSpaceIsDropped() {
        let transcript = VoiceTranscript(tokens: [token(" "), token("iPhone"), token("を", "を")])
        XCTAssertEqual(transcript.parts, [.fixed("iPhone"), .reading("を", surface: "を")])
    }

    func testTypableSymbolsGoIntoReading() {
        // 「？」は打鍵でも読みに入る（RomajiComposer の表）
        let transcript = VoiceTranscript(tokens: [
            token("何時", "なんじ"), token("から", "から"), token("です", "です"), token("か", "か"),
            token(" ？"),
        ])
        XCTAssertEqual(transcript.parts, [.reading("なんじからですか？", surface: "何時からですか？")])
    }

    func testPunctuationStyle() {
        let transcript = VoiceTranscript(
            tokens: [token("はい", "はい"), token("、", "、"), token("そう", "そう"), token("。", "。")],
            comma: "，", period: "．")
        XCTAssertEqual(transcript.parts, [.reading("はい，そう．", surface: "はい、そう。")])
    }

    func testKanjiWithoutReadingStaysFixed() {
        let transcript = VoiceTranscript(tokens: [token("1人"), token("で", "で")])
        XCTAssertEqual(transcript.parts, [.fixed("1人"), .reading("で", surface: "で")])
    }

    func testCounterAfterDigitsStaysFixed() {
        // 数字と切り離して読みを求めると助数詞の読みを取り違える（「10日」の日 → か）
        let transcript = VoiceTranscript(tokens: [
            token("2026"), token("年", "ねん"), token(" "), token("11"), token("月", "がつ"), token(" "),
            token("10"), token("日", "か"), token("立石", "たていし"), token("さん", "さん"), token("と", "と"),
            token(" "), token("3"), token("人", "にん"), token("で", "で"),
        ])
        XCTAssertEqual(transcript.parts, [
            .fixed("2026年"), .fixed("11月"), .fixed("10日"),
            .reading("たていしさんと", surface: "立石さんと"),
            .fixed("3人"), .reading("で", surface: "で"),
        ].merged)
        XCTAssertEqual(transcript.surface, "2026年11月10日立石さんと3人で")
    }

    func testKanaAfterDigitsGoesIntoReading() {
        let transcript = VoiceTranscript(tokens: [token("3"), token("つ", "つ"), token("ある", "ある")])
        XCTAssertEqual(transcript.parts, [.fixed("3"), .reading("つある", surface: "つある")])
    }

    func testEmpty() {
        XCTAssertTrue(VoiceTranscript(tokens: []).isEmpty)
        XCTAssertTrue(VoiceTranscript(tokens: [token(" ")]).isEmpty)
    }
}

private extension Array where Element == VoiceTranscript.Part {
    /// 隣り合う固定部分をつなげる（期待値を語ごとに書けるようにする）
    var merged: [VoiceTranscript.Part] {
        var result: [VoiceTranscript.Part] = []
        for part in self {
            if case .fixed(let text) = part, case .fixed(let previous)? = result.last {
                result[result.count - 1] = .fixed(previous + text)
            } else {
                result.append(part)
            }
        }
        return result
    }
}
