import XCTest
@testable import IrohaCore

final class EraYearCandidatesTests: XCTestCase {

    private func texts(_ katakana: String) -> [String] {
        EraYearCandidates.entries(katakana: katakana).map(\.text)
    }

    func testSeirekiToWareki() {
        XCTAssertEqual(texts("2026ネン"), ["令和8年"])
        XCTAssertEqual(texts("2000ネン"), ["平成12年"])
        XCTAssertEqual(texts("1970ネン"), ["昭和45年"])
        XCTAssertEqual(texts("1920ネン"), ["大正9年"])
        // 改元の年は新旧の元号の 2 つ
        XCTAssertEqual(texts("2019ネン"), ["令和元年", "平成31年"])
        XCTAssertEqual(texts("1989ネン"), ["平成元年", "昭和64年"])
        XCTAssertEqual(texts("1926ネン"), ["昭和元年", "大正15年"])
        XCTAssertEqual(texts("1912ネン"), ["大正元年", "明治45年"])
        XCTAssertEqual(texts("1868ネン"), ["明治元年", "慶應4年"])
    }

    /// azooKey の実装は明治の年で 1967 を引いていて「明治-67年」になる。iroha は 1867 を引く
    func testMeijiYears() {
        XCTAssertEqual(texts("1900ネン"), ["明治33年"])
        XCTAssertEqual(texts("1869ネン"), ["明治2年"])
        XCTAssertEqual(texts("1911ネン"), ["明治44年"])
    }

    func testWarekiToSeireki() {
        XCTAssertEqual(texts("レイワ8ネン"), ["2026年"])
        XCTAssertEqual(texts("ショウワ64ネン"), ["1989年"])
        XCTAssertEqual(texts("ヘイセイ31ネン"), ["2019年"])
        XCTAssertEqual(texts("メイジ33ネン"), ["1900年"])
        XCTAssertEqual(texts("タイショウ9ネン"), ["1920年"])
        XCTAssertEqual(texts("ヘイセイガンネン"), ["1989年"])
        XCTAssertEqual(texts("レイワガンネン"), ["2019年"])
    }

    func testNoCandidatesForOtherReadings() {
        XCTAssertEqual(texts("キョウハ"), [])
        XCTAssertEqual(texts("2026"), [])
        XCTAssertEqual(texts("1800ネン"), [])
        XCTAssertEqual(texts("12ネン"), [])
        // 年の数字は 2 桁まで
        XCTAssertEqual(texts("レイワ100ネン"), [])
        XCTAssertEqual(texts("レイワネン"), [])
    }

    func testValuesMatchAzooKey() {
        let wareki = EraYearCandidates.entries(katakana: "2019ネン")
        XCTAssertEqual(wareki.map(\.value), [-18, -19])
        XCTAssertEqual(wareki.map(\.isWareki), [true, true])
        let seireki = EraYearCandidates.entries(katakana: "レイワ8ネン")
        XCTAssertEqual(seireki.map(\.value), [-15])
        XCTAssertEqual(seireki.map(\.isWareki), [false])
    }
}
