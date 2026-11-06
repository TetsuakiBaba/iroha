import IrohaCore
import XCTest
@testable import IrohaDocuments

final class SystemReadingTokenizerTests: XCTestCase {

    private let tokenizer = SystemReadingTokenizer()

    private func reading(_ text: String) -> String? {
        var result = ""
        for token in tokenizer.tokens(in: text) {
            guard let reading = token.reading else { return nil }
            result += reading
        }
        return result
    }

    func testSurfacesJoinBackToOriginal() {
        let text = "文字単位のT5で、かな漢字変換を行う。 SSD および YOLO"
        XCTAssertEqual(tokenizer.tokens(in: text).map(\.surface).joined(), text)
    }

    func testReadingsKeepKanaDistinctions() {
        // 長音（おお／おう）と助詞（を・は）をローマ字経由でも崩さない
        XCTAssertEqual(reading("大きい道を通る"), "おおきいみちをとおる")
        XCTAssertEqual(reading("東京の経営"), "とうきょうのけいえい")
        XCTAssertEqual(reading("音声干渉の手書き譜面"), "おんせいかんしょうのてがきふめん")
    }

    func testKatakanaIsConvertedDirectly() {
        XCTAssertEqual(SystemReadingTokenizer.reading(surface: "センサー", latin: "sensā"), "せんさー")
    }

    func testPunctuationReadsAsTypedSymbols() {
        XCTAssertEqual(SystemReadingTokenizer.reading(ofPlain: "，"), "、")
        XCTAssertEqual(SystemReadingTokenizer.reading(ofPlain: "．"), "。")
        XCTAssertNil(SystemReadingTokenizer.reading(ofPlain: "T5"))
    }

    func testLatinTokensHaveNoReading() {
        let tokens = tokenizer.tokens(in: "SSDを使う")
        XCTAssertEqual(tokens.first?.surface, "SSD")
        XCTAssertNil(tokens.first?.reading)
    }

    func testCollectionExtractsCompoundTerms() {
        let document = DocumentCollection.makeDocument(
            name: "a.txt", raw: "視覚障害者の屋外移動を支援する。\n参考文献\n[1] 文献", tokenizer: tokenizer)
        let surfaces = Set(document.terms.map(\.surface))
        XCTAssertTrue(surfaces.contains("視覚障害者"))
        XCTAssertTrue(surfaces.contains("屋外移動"))
        XCTAssertFalse(document.text.contains("文献"))
    }
}
