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
        XCTAssertEqual(compose("cyacyucyo"), "ちゃちゅちょ")
        XCTAssertEqual(compose("jajujo"), "じゃじゅじょ")
        XCTAssertEqual(compose("ryaryuryo"), "りゃりゅりょ")
    }

    /// Google日本語入力（Mozc の romanji-hiragana.tsv）にあって iroha に無かった綴り（2026-11-24 に照合して足した）
    func testGoogleJapaneseInputSpellings() {
        let expected: [String: String] = [
            "cya": "ちゃ",
            "cyi": "ちぃ",
            "cyu": "ちゅ",
            "cye": "ちぇ",
            "cyo": "ちょ",
            "xn": "ん",
            "lyi": "ぃ",
            "xyi": "ぃ",
            "lye": "ぇ",
            "xye": "ぇ",
            "lka": "ヵ",
            "lke": "ヶ",
            "xtsu": "っ",
            "vya": "ゔゃ",
            "vyi": "ゔぃ",
            "vyu": "ゔゅ",
            "vye": "ゔぇ",
            "vyo": "ゔょ",
            "hwa": "ふぁ",
            "hwi": "ふぃ",
            "hwe": "ふぇ",
            "hwo": "ふぉ",
            "hwyu": "ふゅ",
            "gyi": "ぎぃ",
            "gye": "ぎぇ",
            "dyi": "ぢぃ",
            "dye": "ぢぇ",
            "hyi": "ひぃ",
            "hye": "ひぇ",
            "byi": "びぃ",
            "bye": "びぇ",
            "pyi": "ぴぃ",
            "pye": "ぴぇ",
            "myi": "みぃ",
            "mye": "みぇ",
            "ryi": "りぃ",
            "rye": "りぇ",
            "jyi": "じぃ",
            "jye": "じぇ",
            "twa": "とぁ",
            "twi": "とぃ",
            "twe": "とぇ",
            "two": "とぉ",
            "dwa": "どぁ",
            "dwi": "どぃ",
            "dwe": "どぇ",
            "dwo": "どぉ",
            "kwa": "くぁ",
            "kwi": "くぃ",
            "kwu": "くぅ",
            "kwe": "くぇ",
            "kwo": "くぉ",
            "gwa": "ぐぁ",
            "gwi": "ぐぃ",
            "gwu": "ぐぅ",
            "gwe": "ぐぇ",
            "gwo": "ぐぉ",
            "swa": "すぁ",
            "swi": "すぃ",
            "swu": "すぅ",
            "swe": "すぇ",
            "swo": "すぉ",
            "zwa": "ずぁ",
            "zwi": "ずぃ",
            "zwu": "ずぅ",
            "zwe": "ずぇ",
            "zwo": "ずぉ",
            "t'i": "てぃ",
            "t'yu": "てゅ",
            "d'i": "でぃ",
            "d'yu": "でゅ",
            "t'u": "とぅ",
            "d'u": "どぅ",
            "wyi": "ゐ",
            "wye": "ゑ",
            "whu": "う",
        ]
        for (romaji, kana) in expected {
            XCTAssertEqual(compose(romaji), kana, romaji)
        }
    }

    /// 足した綴りの頭だけを打ったときは、足す前と同じ結果になる
    func testPartialNewSpellingsKeepPreviousResult() {
        XCTAssertEqual(compose("t'a"), "t'あ")
        XCTAssertEqual(compose("d'a"), "d'あ")
        XCTAssertEqual(compose("kwyo"), "kwよ")
        XCTAssertEqual(compose("hwya"), "hwや")
        XCTAssertEqual(compose("xnn"), "んn")
        XCTAssertEqual(compose("kkwa"), "っくぁ")
    }

    func testSokuon() {
        XCTAssertEqual(compose("katta"), "かった")
        XCTAssertEqual(compose("kippu"), "きっぷ")
        XCTAssertEqual(compose("zasshi"), "ざっし")
        XCTAssertEqual(compose("matcha"), "まっちゃ")
        XCTAssertEqual(compose("maccya"), "まっちゃ")
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

    func testZSymbols() {
        // Google日本語入力（Mozc）と同じもの
        XCTAssertEqual(compose("zhzjzkzl"), "←↓↑→")
        XCTAssertEqual(compose("z.z,z/z-z[z]"), "…‥・〜『』")
        // SKK にだけあるもの
        XCTAssertEqual(compose("z*z0z@z:z;"), "※○◎゜゛")
        XCTAssertEqual(compose("z{z}z(z)zn"), "【】（）ー")
        XCTAssertEqual(compose("az b"), "あ\u{3000}b")
        // 句読点の設定は「z,」「z.」に効かない
        var composer = RomajiComposer(commaText: "，", periodText: "．")
        composer.input("z,z.")
        XCTAssertEqual(composer.display, "‥…")
        // 前後のかな・「ん」・促音とつながる
        XCTAssertEqual(compose("yajirushiz.zl"), "やじるし…→")
        XCTAssertEqual(compose("nzh"), "ん←")
        XCTAssertEqual(compose("zza"), "っざ")
        XCTAssertEqual(compose("zaza"), "ざざ")
        // 「z」だけで確定すれば z のまま
        XCTAssertEqual(compose("az", flush: true), "あz")
        // F9/F10 の英数変換用の打鍵列は残る
        var raw = RomajiComposer()
        raw.input("zh")
        XCTAssertEqual(raw.raw, "zh")
        XCTAssertTrue(raw.rawIsReliable)
    }

    func testZCaseSensitiveKey() {
        var composer = RomajiComposer()
        composer.input("a")
        // 「z」がなければ Shift+L はどのキーも完成させない（コントローラは英字入力を始める）
        XCTAssertFalse(composer.completesKey(with: "L"))
        composer.input("z")
        XCTAssertTrue(composer.completesKey(with: "L"))
        XCTAssertTrue(composer.completesKey(with: " "))
        XCTAssertTrue(composer.completesKey(with: "h"))
        // 大文字は大文字を区別するキーだけ（「z」+ Shift+H は ← にしない）
        XCTAssertFalse(composer.completesKey(with: "H"))
        composer.input("L")
        XCTAssertEqual(composer.display, "あ⇒")
        XCTAssertEqual(composer.raw, "azL")
        // スペースは「z」のあとだけ
        XCTAssertFalse(composer.completesKey(with: " "))
    }

    func testHiraganaToKatakana() {
        XCTAssertEqual(hiraganaToKatakana("にゅうりょく"), "ニュウリョク")
        XCTAssertEqual(hiraganaToKatakana("こーひー"), "コーヒー")
        XCTAssertEqual(hiraganaToKatakana("ゔぁ"), "ヴァ")
    }
}
