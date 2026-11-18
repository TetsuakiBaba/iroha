import XCTest
@testable import IrohaCore

/// 呼び出しを記録するダミーエンジン。
/// 読みを「2文字ごとに漢字1字 + 直後のひらがな」のような形にはできないので、
/// テストしやすいように readingToConversion で結果を差し込めるようにする
private final class RecordingEngine: ConversionEngine, @unchecked Sendable {
    private(set) var calls: [(reading: String, context: String)] = []
    /// 読み → 変換結果（未登録の読みはカタカナにして返す）
    var table: [String: String] = [:]

    func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        calls.append((reading, context))
        return [table[reading] ?? hiraganaToKatakana(reading)]
    }
}

final class ChunkedConversionEngineTests: XCTestCase {

    func testShortReadingPassesThrough() async throws {
        let stub = RecordingEngine()
        let engine = ChunkedConversionEngine(base: stub, maxChunkLength: 10)
        let result = try await engine.convert(reading: "きょうはいいてんき", context: "文脈", candidateCount: 3)
        XCTAssertEqual(result, ["キョウハイイテンキ"])
        XCTAssertEqual(stub.calls.count, 1)
        XCTAssertEqual(stub.calls[0].context, "文脈")
    }

    /// 全角スペースはモデルに渡さず、前後を分けて変換して全角スペースをそのまま戻す
    func testSplitsAtIdeographicSpace() async throws {
        let stub = RecordingEngine()
        stub.table = ["かいし": "開始", "しゅうりょう": "終了"]
        let engine = ChunkedConversionEngine(base: stub, maxChunkLength: 80)
        let result = try await engine.convert(reading: "かいし\u{3000}しゅうりょう", context: "前", candidateCount: 1)
        XCTAssertEqual(result, ["開始\u{3000}終了"])
        XCTAssertEqual(stub.calls.map(\.reading), ["かいし", "しゅうりょう"])
        XCTAssertEqual(stub.calls.map(\.context), ["前", "前開始\u{3000}"])
        // 先頭・末尾・連続した全角スペースもそのまま残る（入力中は末尾に全角スペースがある）
        let edges = try await engine.convert(reading: "\u{3000}かいし\u{3000}\u{3000}", context: "", candidateCount: 1)
        XCTAssertEqual(edges, ["\u{3000}開始\u{3000}\u{3000}"])
    }

    /// 候補を複数求められたら、分けて変換した結果の後ろに、全角スペースの数が合う下のエンジンの候補を続ける
    func testIdeographicSpaceCandidates() async throws {
        let stub = RecordingEngine()
        stub.table = ["かいし": "開始", "しゅうりょう": "終了", "かいし\u{3000}しゅうりょう": "開始🍀終了"]
        let engine = ChunkedConversionEngine(base: stub, maxChunkLength: 80)
        let result = try await engine.convert(reading: "かいし\u{3000}しゅうりょう", context: "", candidateCount: 3)
        XCTAssertEqual(result, ["開始\u{3000}終了"])
        stub.table["かいし\u{3000}しゅうりょう"] = "カイシ\u{3000}終了"
        let more = try await engine.convert(reading: "かいし\u{3000}しゅうりょう", context: "", candidateCount: 3)
        XCTAssertEqual(more, ["開始\u{3000}終了", "カイシ\u{3000}終了"])
    }

    /// 句読点があればその直後で切り、前の区切りの変換結果を次の文脈にする
    func testSplitsAfterPunctuation() async throws {
        let stub = RecordingEngine()
        let engine = ChunkedConversionEngine(base: stub, maxChunkLength: 10)
        // 窓「あいうえお、かきくけ」の中の最後の句読点「、」の直後（6文字）で切る
        let reading = "あいうえお、かきくけこさしすせそ"
        let result = try await engine.convert(reading: reading, context: "前", candidateCount: 1)
        XCTAssertEqual(stub.calls.map(\.reading), ["あいうえお、", "かきくけこさしすせそ"])
        XCTAssertEqual(stub.calls[0].context, "前")
        XCTAssertEqual(stub.calls[1].context, "前アイウエオ、")
        XCTAssertEqual(result, ["アイウエオ、カキクケコサシスセソ"])
    }

    /// 句読点がなければ窓を変換して文節境界で切る。最後の文節は次の区切りへ回す
    func testSplitsAtAlignedSegmentBoundary() async throws {
        let stub = RecordingEngine()
        // 窓1「きょうはてんきがいいので」(12文字) → 文節は [今日は][天気がいいので]
        //   （ReadingAlignerは「漢字 + 直後のかな」を1文節にする）。最後の文節を除いた4文字で切る
        // 窓2「てんきがいいのでさんぽに」 → [天気がいいので][散歩に] → 8文字で切る
        stub.table["きょうはてんきがいいので"] = "今日は天気がいいので"
        stub.table["てんきがいいのでさんぽに"] = "天気がいいので散歩に"
        let engine = ChunkedConversionEngine(base: stub, maxChunkLength: 12)
        let reading = "きょうはてんきがいいのでさんぽにいく"
        let result = try await engine.convert(reading: reading, context: "", candidateCount: 1)
        XCTAssertEqual(stub.calls.map(\.reading),
                       ["きょうはてんきがいいので", "てんきがいいのでさんぽに", "さんぽにいく"])
        XCTAssertEqual(stub.calls.map(\.context), ["", "今日は", "今日は天気がいいので"],
                       "先頭側の区切りの変換結果が次の文脈になる")
        XCTAssertEqual(result, ["今日は天気がいいのでサンポニイク"])
    }

    /// 文節境界が取れなければ窓の長さで切る（変換は1回で済ませる）
    func testFallsBackToWindowLength() async throws {
        let stub = RecordingEngine()
        let engine = ChunkedConversionEngine(base: stub, maxChunkLength: 5)
        // 5文字の窓はカタカナのみ（=全部1文節）で返るので境界が取れない
        let result = try await engine.convert(reading: "あいうえおかきくけこさ", context: "", candidateCount: 1)
        XCTAssertEqual(stub.calls.map(\.reading), ["あいうえお", "かきくけこ", "さ"])
        XCTAssertEqual(result, ["アイウエオカキクケコサ"])
    }

    /// ライブ変換で末尾に1文字ずつ足していくとき、先頭側の区切りは再変換しない
    func testCachesLeadingChunks() async throws {
        let stub = RecordingEngine()
        let engine = ChunkedConversionEngine(base: stub, maxChunkLength: 10)
        _ = try await engine.convert(reading: "あいうえお、かきくけこさし", context: "", candidateCount: 1)
        let before = stub.calls.count
        XCTAssertEqual(stub.calls.map(\.reading), ["あいうえお、", "かきくけこさし"])

        _ = try await engine.convert(reading: "あいうえお、かきくけこさしす", context: "", candidateCount: 1)
        XCTAssertEqual(stub.calls.count, before + 1, "再変換されるのは末尾の区切りだけ")
        XCTAssertEqual(stub.calls.last?.reading, "かきくけこさしす")

        // 同じ読みをもう一度: 全部キャッシュから返る
        _ = try await engine.convert(reading: "あいうえお、かきくけこさしす", context: "", candidateCount: 1)
        XCTAssertEqual(stub.calls.count, before + 1)
    }

    func testPunctuationCutRespectsMinimum() {
        XCTAssertEqual(ChunkedConversionEngine.punctuationCut("あいう、えおかき", minimum: 3), 4)
        XCTAssertNil(ChunkedConversionEngine.punctuationCut("あ、いうえおかき", minimum: 3), "先頭付近の句読点では切らない")
        XCTAssertEqual(ChunkedConversionEngine.punctuationCut("あ、いうえお。かき", minimum: 3), 7, "最後の句読点を使う")
        XCTAssertNil(ChunkedConversionEngine.punctuationCut("あいうえお", minimum: 1))
    }
}
