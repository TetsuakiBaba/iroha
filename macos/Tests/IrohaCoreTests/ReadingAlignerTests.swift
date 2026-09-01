import XCTest
@testable import IrohaCore

final class ReadingAlignerTests: XCTestCase {

    private func segment(_ reading: String, _ conversion: String) -> [(String, String)] {
        ReadingAligner.segmentReading(reading, conversion: conversion).map { ($0.reading, $0.conversion) }
    }

    func testBasicTwoSegments() {
        let result = segment("きょうはいいてんきですね", "今日はいい天気ですね")
        XCTAssertEqual(result.map(\.0), ["きょうはいい", "てんきですね"])
        XCTAssertEqual(result.map(\.1), ["今日はいい", "天気ですね"])
    }

    func testParticleAnchors() {
        let result = segment("さかなをたべる", "魚を食べる")
        XCTAssertEqual(result.map(\.0), ["さかなを", "たべる"])
        XCTAssertEqual(result.map(\.1), ["魚を", "食べる"])
    }

    func testAllKanaIsSingleSegment() {
        let result = segment("すもももももももものうち", "すもももももももものうち")
        XCTAssertEqual(result.count, 1)
    }

    func testAllKanjiIsSingleSegment() {
        let result = segment("かんじへんかん", "漢字変換")
        XCTAssertEqual(result.count, 1)
        XCTAssertEqual(result[0].0, "かんじへんかん")
    }

    func testKatakanaInConversion() {
        // カタカナ部分は読みのひらがなと対応付けられる
        let result = segment("こーひーをのむ", "コーヒーを飲む")
        XCTAssertEqual(result.map(\.1), ["コーヒーを", "飲む"])
        XCTAssertEqual(result.map(\.0), ["こーひーを", "のむ"])
    }

    func testLeadingKana() {
        let result = segment("これはほんです", "これは本です")
        XCTAssertEqual(result.map(\.1), ["これは", "本です"])
    }

    func testMismatchFallsBackToWholeSegment() {
        // 変換結果のかなが読みに現れない場合は分割を諦めて全体を返す
        let result = segment("きょうは", "明日も")
        XCTAssertEqual(result.count, 1)
        XCTAssertEqual(result[0].0, "きょうは")
    }

    func testReadingRoundTrip() {
        // 分割結果の読みを連結すると元の読みに一致する
        let reading = "わたしはがっこうへいきます"
        let segments = ReadingAligner.segmentReading(reading, conversion: "私は学校へ行きます")
        XCTAssertEqual(segments.map(\.reading).joined(), reading)
        XCTAssertEqual(segments.map(\.conversion).joined(), "私は学校へ行きます")
    }

    func testKatakanaToHiragana() {
        XCTAssertEqual(katakanaToHiragana("コーヒー"), "こーひー")
        XCTAssertEqual(katakanaToHiragana("ヴァイオリン"), "ゔぁいおりん")
    }
}
