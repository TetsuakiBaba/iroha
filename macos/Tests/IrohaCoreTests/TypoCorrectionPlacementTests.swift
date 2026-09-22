import XCTest
@testable import IrohaCore

/// 読み全体の訂正を文節ごとの候補に落とす計算（モデルは読まないので常に走る）
final class TypoCorrectionPlacementTests: XCTestCase {

    private func correction(_ reading: String, _ corrected: String) -> TypoCorrection {
        TypoCorrection(reading: reading, corrected: corrected, margin: 5)
    }

    /// 差分は前後の一致部分を取り除いた最小の範囲になる（置換・削除・挿入）
    func testEditRange() {
        let replaced = correction("をわぇてけいやくする", "をわけてけいやくする").edit
        XCTAssertEqual(replaced.range, 2..<3)
        XCTAssertEqual(replaced.replacement, "け")

        let deleted = correction("がちがうんっだろな", "がちがうんだろな").edit
        XCTAssertEqual(deleted.range, 5..<6)
        XCTAssertEqual(deleted.replacement, "")

        let inserted = correction("とうきょうえき", "とうきょうのえき").edit
        XCTAssertEqual(inserted.range, 5..<5)
        XCTAssertEqual(inserted.replacement, "の")
    }

    /// 差分が収まっている文節にだけ訂正後の読みが出る
    func testPlacementInsideOneSegment() {
        let result = correction("をわぇてけいやくする", "をわけてけいやくする")
            .placement(inSegments: ["をわぇて", "けいやくする"])
        XCTAssertEqual(result?.index, 0)
        XCTAssertEqual(result?.correctedReading, "をわけて")
    }

    func testPlacementInLaterSegment() {
        let result = correction("きょうはあめだっだ", "きょうはあめだった")
            .placement(inSegments: ["きょうは", "あめだっだ"])
        XCTAssertEqual(result?.index, 1)
        XCTAssertEqual(result?.correctedReading, "あめだった")
    }

    /// 文節境界をまたぐ差分（入れ替わりが境界をはさむ）は見送る
    func testPlacementAcrossBoundaryIsRejected() {
        // 「はあ」→「あは」の入れ替えは「きょうは」と「あめ」の境界をまたぐ
        let result = correction("きょうはあめ", "きょうあはめ")
            .placement(inSegments: ["きょうは", "あめ"])
        XCTAssertNil(result)
    }

    /// 文節の読みをつないだものが元の読みと違えば（分割し直した後など）何も返さない
    func testPlacementRequiresMatchingSegments() {
        let result = correction("をわぇてけいやくする", "をわけてけいやくする")
            .placement(inSegments: ["まったくちがうよみ"])
        XCTAssertNil(result)
    }

    /// 先頭に訂正対象外の固定文節（英字・取り入れた予測）があっても対応づけられる。
    /// 訂正は未確定の読み全体にだけ出るので、文節の列とは長さが合わない
    func testPlacementSkipsFixedPrefixSegments() {
        let result = correction("をわぇてけいやくする", "をわけてけいやくする")
            .placement(inSegmentsWithFixedPrefix: ["USB", "をわぇて", "けいやくする"])
        XCTAssertEqual(result?.index, 1)
        XCTAssertEqual(result?.correctedReading, "をわけて")
    }

    /// 固定部分を含めても読みが一致しなければ何も出さない（文節を切り直した後など）
    func testPlacementWithFixedPrefixRequiresMatch() {
        let result = correction("をわぇてけいやくする", "をわけてけいやくする")
            .placement(inSegmentsWithFixedPrefix: ["USB", "べつのよみ"])
        XCTAssertNil(result)
    }

    /// 固定部分が無いときは通常版と同じ結果になる
    func testPlacementWithFixedPrefixMatchesPlainVersion() {
        let segments = ["きょうは", "あめだっだ"]
        let fix = correction("きょうはあめだっだ", "きょうはあめだった")
        let plain = fix.placement(inSegments: segments)
        let withPrefix = fix.placement(inSegmentsWithFixedPrefix: segments)
        XCTAssertEqual(plain?.index, withPrefix?.index)
        XCTAssertEqual(plain?.correctedReading, withPrefix?.correctedReading)
    }

    /// 「末尾に足しただけ」の訂正は入力中には使わない（訂正ではなく続きの補完なので）
    func testTrailingInsertionOnly() {
        // モデルが句読点で文を締めようとするパターン（実測した誤検出の大半がこれ）
        XCTAssertTrue(correction("こえて", "こえて、").isTrailingInsertionOnly)
        XCTAssertTrue(correction("やはり", "やはり。").isTrailingInsertionOnly)
        XCTAssertTrue(correction("いゔ", "いゔー").isTrailingInsertionOnly)
        // 途中を直すものは対象外（本来の打ち間違いの訂正）
        XCTAssertFalse(correction("がちがうんっだろな", "がちがうんだろな").isTrailingInsertionOnly)
        XCTAssertFalse(correction("をわぇてけいやくする", "をわけてけいやくする").isTrailingInsertionOnly)
        XCTAssertFalse(correction("とらは", "とらいは").isTrailingInsertionOnly)
        // 末尾の文字を「置き換える」のは補完ではないので残す
        XCTAssertFalse(correction("こえてを", "こえてほ").isTrailingInsertionOnly)
        // 末尾を削るのも補完ではない
        XCTAssertFalse(correction("こえてを", "こえて").isTrailingInsertionOnly)
    }

    /// 差分が文節境界をまたぐときは、訂正対象の先頭文節の位置がわかる
    /// （文全体を訂正した候補をそこに出すため）
    func testSegmentOffsetForStraddlingEdit() {
        let fix = correction("さsてえいただいていて", "させていただいていて")
        let segments = ["さ", "sて", "えいただいていて"]
        XCTAssertNil(fix.placement(inSegments: segments), "またいでいるので1文節では直せない")
        XCTAssertEqual(fix.segmentOffset(inSegments: segments), 0)
    }

    /// 先頭に固定文節があれば、その後ろが訂正対象の先頭になる
    func testSegmentOffsetWithFixedPrefix() {
        let fix = correction("さsてえいただいていて", "させていただいていて")
        XCTAssertEqual(fix.segmentOffset(inSegments: ["USB", "さ", "sて", "えいただいていて"]), 1)
    }

    /// 読みがどうつないでも一致しなければ位置は出ない
    func testSegmentOffsetRequiresMatch() {
        let fix = correction("さsてえいただいていて", "させていただいていて")
        XCTAssertNil(fix.segmentOffset(inSegments: ["まったく", "ちがう"]))
    }

    /// 文節の端での挿入は、その位置を含む文節に付く（どちらに付いても読み全体は同じになる）
    func testInsertionAtSegmentBoundary() {
        let result = correction("とうきょうえき", "とうきょうのえき")
            .placement(inSegments: ["とうきょう", "えき"])
        XCTAssertEqual(result?.index, 0)
        XCTAssertEqual(result?.correctedReading, "とうきょうの")
    }
}
