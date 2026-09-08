import XCTest
@testable import IrohaCore

private struct FixedEngine: ConversionEngine {
    var results: [String]
    func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        Array(results.prefix(max(candidateCount, 1)))
    }
}

final class VariantKanjiEngineTests: XCTestCase {

    /// 候補ウィンドウでは異体字を末尾に足し、既にあるものは重複させない
    func testAppendsVariantsWithoutDuplicates() async throws {
        let engine = VariantKanjiEngine(base: FixedEngine(results: ["崎", "先", "嵜"]))
        let result = try await engine.convert(reading: "さき", context: "", candidateCount: 8)
        XCTAssertEqual(result, ["崎", "先", "嵜", "﨑"])
    }

    /// ライブ変換（候補数1）には影響しない
    func testDoesNotAffectFirstCandidate() async throws {
        let engine = VariantKanjiEngine(base: FixedEngine(results: ["高"]))
        let result = try await engine.convert(reading: "たか", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["高"])
    }

    /// 読みはカタカナでも照合できる。表にない読みは何も足さない
    func testReadingNormalizationAndUnknownReading() async throws {
        XCTAssertEqual(VariantKanjiTable.variants(forReading: "タカ"), ["髙"])
        XCTAssertEqual(VariantKanjiTable.variants(forReading: "きしゃ"), [])
    }
}
