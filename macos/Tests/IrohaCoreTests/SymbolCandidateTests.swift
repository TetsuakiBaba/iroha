import XCTest
@testable import IrohaCore

final class SymbolCandidateTests: XCTestCase {

    // MARK: - 表

    private static let symbolTSV = """
        POS\tCHAR\tReading (space separated)\tdescription\tadditional description\tcategory\tmemo\tunicode
        記号\t→\tやじるし みぎ ->\t右矢印\t\tSYMBOL
        記号\t←\tやじるし ひだり\t左矢印\t\tSYMBOL
        記号\t☺\tえがお かおもじ :)\t顔文字
        """
    private static let emoticonTSV = """
        \tkeys (space separated)\tcategories (space separated)
        (^^)\tにこにこ にこ\tSMILE
        (;_;)\tなく\tCRY
        """
    private static let categorizedTSV = """
        # value\tcategory\tdescription
        (^^)\tSMILE\tにこにこ
        (๑´ڡ`๑)\tSMILE\tぺろ おいしい
        """
    private static let emojiTSV = """
        # comment
        1F602\t😂\tわらい なきわらい\t\t絵文字\t\tE0.6
        1F62D\t😭\tなく\t\t絵文字\t\tE0.6
        1FAE9\t🫩\tねむい\t\t絵文字\t\tE16.0
        """

    private func makeTable(isRenderable: (String) -> Bool = { _ in true }) -> SymbolCandidateTable {
        SymbolCandidateTable(symbolTSV: Self.symbolTSV, emoticonTSV: Self.emoticonTSV,
                             categorizedTSV: Self.categorizedTSV, emojiTSV: Self.emojiTSV,
                             isRenderableEmoji: isRenderable)
    }

    /// 読みごとに 3 種類を分けて引ける。かな以外の読み（「->」「:)」）は使わない。カタカナの読みでも引ける
    func testLookup() {
        let table = makeTable()
        XCTAssertEqual(table.candidates(forReading: "やじるし").symbols, ["→", "←"])
        XCTAssertEqual(table.candidates(forReading: "なく"),
                       SymbolCandidates(emoji: ["😭"], emoticons: ["(;_;)"], symbols: []))
        XCTAssertEqual(table.candidates(forReading: "ワライ").emoji, ["😂"])
        XCTAssertTrue(table.candidates(forReading: "->").isEmpty)
        XCTAssertTrue(table.candidates(forReading: "きしゃ").isEmpty)
    }

    /// 2 つの顔文字ファイルに同じ顔文字があっても 1 つにまとめる
    func testEmoticonsAreDeduplicated() {
        let table = makeTable()
        XCTAssertEqual(table.candidates(forReading: "にこにこ").emoticons, ["(^^)"])
        XCTAssertEqual(table.candidates(forReading: "かおもじ").emoticons, ["(^^)", "(;_;)", "(๑´ڡ`๑)"])
        // 「かおもじ」の読みを持つ記号は記号として出る
        XCTAssertEqual(table.candidates(forReading: "かおもじ").symbols, ["☺"])
    }

    /// 「えもじ」で全部の絵文字。描けない絵文字は表に入れない
    func testAllEmojiAndRenderability() {
        XCTAssertEqual(makeTable().candidates(forReading: "えもじ").emoji, ["😂", "😭", "🫩"])
        let table = makeTable { $0 != "🫩" }
        XCTAssertEqual(table.candidates(forReading: "えもじ").emoji, ["😂", "😭"])
        XCTAssertTrue(table.candidates(forReading: "ねむい").isEmpty)
    }

    // MARK: - 合流

    private let engine = ["笑い", "わらい", "嗤い", "ワライ", "藁位", "わら位", "割らい", "笑", "🐱"]

    /// 絵文字は 7 番目、顔文字・記号はエンジンの候補の後ろ
    func testEmojiAtSeventhAndOthersAfterEngine() {
        let symbols = SymbolCandidates(emoji: ["😂", "😁"], emoticons: ["(^^)"], symbols: ["☺"])
        let merged = SymbolCandidateMerger.merge(engine, with: symbols, protectedCount: 1)
        XCTAssertEqual(merged, ["笑い", "わらい", "嗤い", "ワライ", "藁位", "わら位", "😂", "😁",
                                "割らい", "笑", "🐱", "(^^)", "☺"])
    }

    /// エンジンが出していた絵文字は絵文字の並びへ移す。記号は元の位置のまま
    func testExistingEmojiMovesAndExistingSymbolStays() {
        let symbols = SymbolCandidates(emoji: ["🐈", "🐱"], symbols: ["わらい"])
        let merged = SymbolCandidateMerger.merge(engine, with: symbols, protectedCount: 1)
        XCTAssertEqual(merged, ["笑い", "わらい", "嗤い", "ワライ", "藁位", "わら位", "🐈", "🐱",
                                "割らい", "笑"])
    }

    /// 動かさない候補（今の表示・変換ルール・訂正）が 7 番目より下まであれば、その後ろに入れる。
    /// 動かさない範囲にある絵文字はそのまま
    func testProtectedHeadIsKept() {
        let candidates = ["😂", "a", "b", "c", "d", "e", "f", "g", "h"]
        let symbols = SymbolCandidates(emoji: ["😂", "😁"])
        let merged = SymbolCandidateMerger.merge(candidates, with: symbols, protectedCount: 8)
        XCTAssertEqual(merged, ["😂", "a", "b", "c", "d", "e", "f", "g", "😁", "h"])
    }

    /// 候補が 7 件に満たなければ末尾に入れる
    func testShortCandidateList() {
        let merged = SymbolCandidateMerger.merge(["猫", "ねこ"], with: SymbolCandidates(emoji: ["🐱"]),
                                                 protectedCount: 1)
        XCTAssertEqual(merged, ["猫", "ねこ", "🐱"])
    }

    /// 学習しないのは表に載っていてこの読みで出た候補。漢字・かなだけの候補は普通の語として学習してよい
    func testUnlearnableCandidates() {
        let symbols = SymbolCandidates(emoji: ["😂"], emoticons: ["(^^)"], symbols: ["女", "♀", "ゃ"])
        let candidates = ["女", "おんな", "♀", "😂", "ゃ"]
        XCTAssertEqual(SymbolCandidateMerger.unlearnableCandidates(in: candidates, symbols: symbols),
                       ["♀", "😂"])
    }

    // MARK: - 実データ

    /// Mozc のデータ（fetch-symbols.sh で取得）が読めて、前に足りなかった候補が出る
    func testBundledData() throws {
        guard let directory = SymbolCandidateTable.defaultDirectoryURL(),
              let table = SymbolCandidateTable(directory: directory) else {
            throw XCTSkip("記号のデータが無いためスキップ（macos/scripts/fetch-symbols.sh で取得）")
        }
        XCTAssertTrue(table.candidates(forReading: "わらい").emoji.contains("😂"))
        XCTAssertTrue(table.candidates(forReading: "ぎりしゃ").symbols.contains("α"))
        XCTAssertTrue(table.candidates(forReading: "ごめん").emoticons.contains("m(_ _)m"))
        XCTAssertGreaterThan(table.counts.emoji, 1500)
        XCTAssertGreaterThan(table.counts.emoticons, 400)
    }
}
