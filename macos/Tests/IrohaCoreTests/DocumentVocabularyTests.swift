import XCTest
@testable import IrohaCore

final class DocumentVocabularyTests: XCTestCase {

    // MARK: - 文字列を整える

    func testNormalizeJoinsJapaneseAcrossLineBreaks() {
        XCTAssertEqual(DocumentText.normalize("音声リアクション・コメント読み上げ共有システ\nムを開発した"),
                       "音声リアクション・コメント読み上げ共有システムを開発した")
    }

    func testNormalizeRemovesSpacesTouchingJapanese() {
        XCTAssertEqual(DocumentText.normalize("馬 場 哲 晃"), "馬場哲晃")
        XCTAssertEqual(DocumentText.normalize("2020 年度から SSD および YOLO を"), "2020年度からSSDおよびYOLOを")
    }

    func testNormalizeKeepsSpacesBetweenLatinWords() {
        XCTAssertEqual(DocumentText.normalize("Object Detection\nDataset"), "Object Detection Dataset")
    }

    func testSentencesSplitAtTerminators() {
        XCTAssertEqual(DocumentText.sentences(in: "本稿では提案する．評価した。まとめ"),
                       ["本稿では提案する．", "評価した。", "まとめ"])
    }

    func testRemovingReferencesCutsOnlyTheTrailingSection() {
        let body = String(repeating: "本文。", count: 20)
        XCTAssertEqual(DocumentText.removingReferences(body + "参考文献 [1] 某"), body)
        // 前半で触れただけの「参考文献」は節の見出しとみなさない
        let early = "参考文献を調べた。" + body
        XCTAssertEqual(DocumentText.removingReferences(early), early)
    }

    // MARK: - 語を取り出す

    private func tokens(_ pairs: [(String, String?)]) -> [ReadingToken] {
        pairs.map { ReadingToken(surface: $0.0, reading: $0.1) }
    }

    func testTermsAreRunsOfKanjiAndKatakanaTokens() {
        let terms = DocumentTermExtractor.terms(in: tokens([
            ("視覚", "しかく"), ("障害", "しょうがい"), ("者", "しゃ"), ("の", "の"), ("支援", "しえん"),
        ]))
        let pairs = Set(terms.map { "\($0.surface)/\($0.reading)" })
        XCTAssertEqual(pairs, [
            "視覚/しかく", "視覚障害/しかくしょうがい", "視覚障害者/しかくしょうがいしゃ",
            "障害/しょうがい", "障害者/しょうがいしゃ", "支援/しえん",
        ])
    }

    func testSingleCharacterTokensAloneAreNotTerms() {
        let terms = DocumentTermExtractor.terms(in: tokens([("者", "しゃ"), ("が", "が"), ("値", "あたい")]))
        XCTAssertTrue(terms.isEmpty)
    }

    func testTokensWithoutReadingOrWithLatinBreakRuns() {
        let terms = DocumentTermExtractor.terms(in: tokens([
            ("物体", "ぶったい"), ("SSD", nil), ("検出", "けんしゅつ"),
        ]))
        XCTAssertEqual(terms.map(\.surface), ["物体", "検出"])
    }

    func testKatakanaTermsKeepLongVowel() {
        let terms = DocumentTermExtractor.terms(in: tokens([("センサー", "せんさー"), ("値", "あたい")]))
        XCTAssertTrue(terms.contains(DocumentTermOccurrence(surface: "センサー値", reading: "せんさーあたい")))
    }

    // MARK: - 一覧

    func testVocabularyMergesCountsAndDocuments() {
        let a = DocumentTermOccurrence(surface: "譜面", reading: "ふめん")
        let b = DocumentTermOccurrence(surface: "物体検出", reading: "ぶったいけんしゅつ")
        let vocabulary = DocumentVocabulary(occurrences: [("x.pdf", [a, a, b]), ("y.pdf", [b, b])])
        XCTAssertEqual(vocabulary.terms.map(\.surface), ["物体検出", "譜面"])
        XCTAssertEqual(vocabulary.terms[0].count, 3)
        XCTAssertEqual(vocabulary.terms[0].documents, ["x.pdf", "y.pdf"])

        let withoutY = vocabulary.excluding(document: "y.pdf", occurrences: [b, b])
        XCTAssertEqual(withoutY.terms.map(\.surface), ["譜面", "物体検出"])
        XCTAssertEqual(withoutY.terms.first { $0.surface == "物体検出" }?.documents, ["x.pdf"])
        XCTAssertNil(vocabulary.excluding(document: "x.pdf", occurrences: [a, a, b]).terms.first { $0.surface == "譜面" })
    }
}
