import XCTest
@testable import IrohaCore

/// ZenzEngineの生成バイト列のデコード（モデル不要の純粋関数）
final class ZenzEngineDecodeTests: XCTestCase {

    func testDecodesValidUTF8Unchanged() {
        let text = "今日はいい天気ですね😀"
        XCTAssertEqual(ZenzEngine.decodeUTF8DroppingFragments(Data(text.utf8)), text)
        XCTAssertEqual(ZenzEngine.decodeUTF8DroppingFragments(Data()), "")
    }

    /// 制約なし生成で多バイト文字の途中で止まった場合、末尾の断片を捨てて残りを返す
    func testDropsTrailingIncompleteSequence() {
        let base = Data("準備いたし".utf8)
        let fragment = Data("ま".utf8).prefix(2)   // 3バイト文字の先頭2バイト
        XCTAssertEqual(ZenzEngine.decodeUTF8DroppingFragments(base + fragment), "準備いたし")
        let fragment4 = Data("😀".utf8).prefix(3)  // 4バイト文字の先頭3バイト
        XCTAssertEqual(ZenzEngine.decodeUTF8DroppingFragments(base + fragment4), "準備いたし")
    }

    /// 途中に不正なバイトが混ざっていてもエラーにせず、その部分だけ落とす
    func testDropsInvalidBytesInTheMiddle() {
        var bytes = Data("資料".utf8)
        bytes.append(0xFF)
        bytes.append(Data("に".utf8))
        XCTAssertEqual(ZenzEngine.decodeUTF8DroppingFragments(bytes), "資料に")
    }
}
