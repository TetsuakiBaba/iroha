import XCTest
@testable import IrohaCore

/// 読みを1文字ずつ「漢」に変え、各文字にマージン0.2を付ける疑似エンジン（自信度の伝播の確認用）
private struct UncertainEngine: ConversionEngine {
    func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        [try await convertScored(reading: reading, context: context).text]
    }

    func convertScored(reading: String, context: String) async throws -> ScoredConversion {
        ScoredConversion(
            text: String(repeating: "漢", count: reading.count), logProb: -1,
            confidences: Array(repeating: CharacterConfidence(logProb: -0.5, margin: 0.2, relaxed: false),
                               count: reading.count))
    }
}

/// 自信度を出さないエンジン（既定実装が全文字を信頼済みにする）
private struct PlainEngine: ConversionEngine {
    func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        [String(repeating: "字", count: reading.count)]
    }
}

final class ScoredConversionTests: XCTestCase {
    func testDefaultImplementationTrustsEveryCharacter() async throws {
        let scored = try await PlainEngine().convertScored(reading: "あいう", context: "")
        XCTAssertEqual(scored.text, "字字字")
        XCTAssertEqual(scored.confidences, Array(repeating: .trusted, count: 3))
        XCTAssertEqual(scored.lowConfidenceRanges(marginBelow: 1.0), [])
    }

    func testLowConfidenceRangesMergeNeighbours() {
        let low = CharacterConfidence(logProb: -1, margin: 0.3, relaxed: false)
        let scored = ScoredConversion(text: "あいうえお", logProb: 0,
                                      confidences: [low, low, .trusted, low, .trusted])
        XCTAssertEqual(scored.lowConfidenceRanges(marginBelow: 1.0), [0..<2, 3..<4])
        // 制約を緩めた文字はマージンに関わらず含める
        let relaxed = CharacterConfidence(logProb: 0, margin: .infinity, relaxed: true)
        let scored2 = ScoredConversion(text: "あ", logProb: 0, confidences: [relaxed])
        XCTAssertEqual(scored2.lowConfidenceRanges(marginBelow: 1.0), [0..<1])
    }

    func testUserDictionaryWordsAreTrustedAndEngineOutputKeepsConfidence() async throws {
        let dictionary = UserDictionary(entries: [UserDictionaryEntry(reading: "きらら", word: "雲母")])
        let engine = UserDictionaryEngine(base: UncertainEngine(), dictionary: { dictionary })
        let scored = try await engine.convertScored(reading: "きららにいく", context: "")
        XCTAssertEqual(scored.text, "雲母漢漢漢")
        XCTAssertEqual(scored.confidences.count, scored.text.count)
        XCTAssertEqual(scored.confidences.prefix(2).map(\.margin), [.infinity, .infinity])
        XCTAssertEqual(scored.lowConfidenceRanges(marginBelow: 1.0), [2..<5])
        // 従来の convert と同じ文字列
        let plain = try await engine.convert(reading: "きららにいく", context: "", candidateCount: 1)
        XCTAssertEqual(plain, [scored.text])
    }

    func testLearningResultsAreTrusted() async throws {
        let learning = LearningDictionary(entries: [
            LearningEntry(kind: .segment, reading: "きしゃ", result: "記者", leftContext: ""),
        ])
        let engine = LearningEngine(base: UncertainEngine(), dictionary: { learning })
        let scored = try await engine.convertScored(reading: "きしゃのかえり", context: "")
        XCTAssertEqual(scored.text, "記者漢漢漢漢")
        XCTAssertEqual(scored.confidences.count, scored.text.count)
        XCTAssertEqual(scored.lowConfidenceRanges(marginBelow: 1.0), [2..<6])
    }

    func testChunkedConversionConcatenatesConfidences() async throws {
        let engine = ChunkedConversionEngine(base: UncertainEngine(), maxChunkLength: 6)
        let reading = "あいう、えおか、きくけこ"
        let scored = try await engine.convertScored(reading: reading, context: "")
        XCTAssertEqual(scored.text.count, reading.count)
        XCTAssertEqual(scored.confidences.count, reading.count)
        XCTAssertEqual(scored.lowConfidenceRanges(marginBelow: 1.0), [0..<reading.count])
    }
}
