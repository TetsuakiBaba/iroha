import XCTest
@testable import IrohaCore

final class RomajiComposerTests: XCTestCase {

    private func compose(_ input: String, flush: Bool = false) -> String {
        var composer = RomajiComposer()
        composer.input(input)
        if flush { composer.flush() }
        return composer.display
    }

    func testBasicGojuon() {
        XCTAssertEqual(compose("aiueo"), "あいうえお")
        XCTAssertEqual(compose("kakikukeko"), "かきくけこ")
        XCTAssertEqual(compose("sashisuseso"), "さしすせそ")
        XCTAssertEqual(compose("tachitsuteto"), "たちつてと")
        XCTAssertEqual(compose("naninuneno"), "なにぬねの")
        XCTAssertEqual(compose("hahifuheho"), "はひふへほ")
        XCTAssertEqual(compose("mamimumemo"), "まみむめも")
        XCTAssertEqual(compose("yayuyo"), "やゆよ")
        XCTAssertEqual(compose("rarirurero"), "らりるれろ")
        XCTAssertEqual(compose("wawo"), "わを")
    }

    func testDakutenHandakuten() {
        XCTAssertEqual(compose("gagigugego"), "がぎぐげご")
        XCTAssertEqual(compose("zajizuzezo"), "ざじずぜぞ")
        XCTAssertEqual(compose("dadedo"), "だでど")
        XCTAssertEqual(compose("babibubebo"), "ばびぶべぼ")
        XCTAssertEqual(compose("papipupepo"), "ぱぴぷぺぽ")
    }

    func testYouon() {
        XCTAssertEqual(compose("kyakyukyo"), "きゃきゅきょ")
        XCTAssertEqual(compose("shashusho"), "しゃしゅしょ")
        XCTAssertEqual(compose("chachucho"), "ちゃちゅちょ")
        XCTAssertEqual(compose("jajujo"), "じゃじゅじょ")
        XCTAssertEqual(compose("ryaryuryo"), "りゃりゅりょ")
    }

    func testSokuon() {
        XCTAssertEqual(compose("katta"), "かった")
        XCTAssertEqual(compose("kippu"), "きっぷ")
        XCTAssertEqual(compose("zasshi"), "ざっし")
        XCTAssertEqual(compose("matcha"), "まっちゃ")
        XCTAssertEqual(compose("ltu"), "っ")
        XCTAssertEqual(compose("xtu"), "っ")
    }

    func testHatsuon() {
        // nn は「ん」（両方消費）
        XCTAssertEqual(compose("kanni"), "かんい")
        // n + 子音 → ん
        XCTAssertEqual(compose("kanji"), "かんじ")
        XCTAssertEqual(compose("sinbun"), "しんぶn")
        XCTAssertEqual(compose("sinbun", flush: true), "しんぶん")
        // n' → ん
        XCTAssertEqual(compose("kin'en", flush: true), "きんえん")
        // 単独 n は次入力待ち、flushで確定
        XCTAssertEqual(compose("n"), "n")
        XCTAssertEqual(compose("n", flush: true), "ん")
        // n + 母音は な行
        XCTAssertEqual(compose("nano"), "なの")
    }

    func testSmallVowels() {
        XCTAssertEqual(compose("la"), "ぁ")
        XCTAssertEqual(compose("xyaxyuxyo"), "ゃゅょ")
        XCTAssertEqual(compose("fasi"), "ふぁし")
        XCTAssertEqual(compose("thi"), "てぃ")
        XCTAssertEqual(compose("dhi"), "でぃ")
        XCTAssertEqual(compose("vaiorin"), "ゔぁいおりn")
    }

    func testSymbols() {
        XCTAssertEqual(compose("ko-hi-"), "こーひー")
        XCTAssertEqual(compose("a,b."), "あ、b。")
        XCTAssertEqual(compose("!?"), "！？")
        XCTAssertEqual(compose("[a]"), "「あ」")
    }

    func testPendingDisplay() {
        XCTAssertEqual(compose("k"), "k")
        XCTAssertEqual(compose("ky"), "ky")
        XCTAssertEqual(compose("kya"), "きゃ")
        XCTAssertEqual(compose("sh"), "sh")
    }

    func testDeleteBackward() {
        var composer = RomajiComposer()
        composer.input("kak")
        XCTAssertEqual(composer.display, "かk")
        composer.deleteBackward()
        XCTAssertEqual(composer.display, "か")
        composer.deleteBackward()
        XCTAssertEqual(composer.display, "")
        XCTAssertTrue(composer.isEmpty)
        // 拗音は表示単位で1文字ずつ消える
        composer.input("kya")
        XCTAssertEqual(composer.display, "きゃ")
        composer.deleteBackward()
        XCTAssertEqual(composer.display, "き")
    }

    func testPrependText() {
        // 固定表示（F6/F7等）を解除して読みに戻すときの挿入
        var composer = RomajiComposer()
        composer.input("totemok")
        composer.prependText("きょうは")
        XCTAssertEqual(composer.display, "きょうはとてもk")
        XCTAssertEqual(composer.text, "きょうはとても")
        XCTAssertFalse(composer.rawIsReliable)
        // 挿入した読みも1文字ずつ消せる
        for _ in 0..<4 { composer.deleteBackward() }
        XCTAssertEqual(composer.display, "きょうは")
        // 空文字の挿入は何もしない
        var empty = RomajiComposer()
        empty.prependText("")
        XCTAssertTrue(empty.isEmpty)
        XCTAssertTrue(empty.rawIsReliable)
    }

    func testPassthroughUnknown() {
        // 数字はそのまま通す。"b2" は b が解決不能になった時点で文字として出力される
        XCTAssertEqual(compose("a1b2", flush: true), "あ1b2")
    }

    func testRealWords() {
        XCTAssertEqual(compose("kyouhaiitenki"), "きょうはいいてんき")
        XCTAssertEqual(compose("konnnichiha"), "こんにちは")
        XCTAssertEqual(compose("gakkou"), "がっこう")
        XCTAssertEqual(compose("nihongonyuuryoku"), "にほんごにゅうりょく")
    }

    func testPunctuationStyle() {
        var composer = RomajiComposer(commaText: "，", periodText: "．")
        composer.input("a,b.")
        composer.flush()
        XCTAssertEqual(composer.display, "あ，b．")
        // デフォルトは「、。」
        XCTAssertEqual(compose("a,b.", flush: true), "あ、b。")
    }

    func testHiraganaToKatakana() {
        XCTAssertEqual(hiraganaToKatakana("にゅうりょく"), "ニュウリョク")
        XCTAssertEqual(hiraganaToKatakana("こーひー"), "コーヒー")
        XCTAssertEqual(hiraganaToKatakana("ゔぁ"), "ヴァ")
    }
}
