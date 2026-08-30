import XCTest
@testable import IrohaCore

/// 呼び出しを記録するだけのダミーエンジン（読みをカタカナにして返す）
private final class StubEngine: ConversionEngine, @unchecked Sendable {
    private(set) var calls: [(reading: String, context: String, count: Int)] = []
    var shouldFail = false

    func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        calls.append((reading, context, candidateCount))
        if shouldFail { throw ConversionError.inferenceFailed("テスト") }
        return (0..<max(1, candidateCount)).map { index in
            index == 0 ? hiraganaToKatakana(reading) : "\(hiraganaToKatakana(reading))\(index)"
        }
    }
}

final class LearningEngineTests: XCTestCase {

    private func makeEngine(_ entries: [LearningEntry]) -> (LearningEngine, StubEngine) {
        let stub = StubEngine()
        let dictionary = LearningDictionary(entries: entries)
        return (LearningEngine(base: stub, dictionary: { dictionary }), stub)
    }

    private func segment(_ reading: String, _ result: String, context: String = "") -> LearningEntry {
        LearningEntry(kind: .segment, reading: reading, result: result, leftContext: context)
    }

    func testEmptyLearningPassesThrough() async throws {
        let (engine, stub) = makeEngine([])
        let result = try await engine.convert(reading: "きしゃ", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["キシャ"])
        XCTAssertEqual(stub.calls.count, 1)
    }

    func testSentenceMatchSkipsEngine() async throws {
        let (engine, stub) = makeEngine([
            LearningEntry(kind: .sentence, reading: "きしゃのきしゃ", result: "記者の貴社")
        ])
        let result = try await engine.convert(reading: "きしゃのきしゃ", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["記者の貴社"])
        XCTAssertTrue(stub.calls.isEmpty, "入力全体が一致すればエンジンを呼ばない")
    }

    /// 同じ読みでも位置によって違う変換を再現する（この機能の肝）
    func testSegmentsChainWithContext() async throws {
        let (engine, stub) = makeEngine([
            segment("きしゃの", "記者の"),
            segment("きしゃ", "貴社", context: "記者の"),
        ])
        let result = try await engine.convert(
            reading: "きしゃのきしゃがきた", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["記者の貴社ガキタ"])
        // 学習で埋められなかった「がきた」だけがエンジンに渡る
        XCTAssertEqual(stub.calls.map(\.reading), ["がきた"])
        XCTAssertEqual(stub.calls[0].context, "記者の貴社")
    }

    /// 文脈なしで覚えた文節を文中で無差別に当てはめない（「あのきしゃ」が「あの記者」になるのを防ぐ）
    func testContextlessEntryDoesNotFireMidSentence() async throws {
        let (engine, stub) = makeEngine([segment("きしゃ", "記者")])
        let result = try await engine.convert(reading: "あのきしゃ", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["アノキシャ"])
        XCTAssertEqual(stub.calls.count, 1, "読み全体を1回で変換する（分割しない）")
        XCTAssertEqual(stub.calls[0].reading, "あのきしゃ")
    }

    func testContextlessEntryFiresAtStart() async throws {
        let (engine, stub) = makeEngine([segment("きしゃ", "記者")])
        let result = try await engine.convert(reading: "きしゃがきた", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["記者ガキタ"])
        XCTAssertEqual(stub.calls.map(\.reading), ["がきた"])
    }

    func testContextMismatchFallsBackToEngine() async throws {
        let (engine, stub) = makeEngine([segment("きしゃ", "貴社", context: "記者の")])
        let result = try await engine.convert(reading: "あのきしゃ", context: "", candidateCount: 1)
        // 文脈が合わないので学習結果は使わない（読みは分割されるが内容はエンジンの変換）
        XCTAssertEqual(result, ["アノキシャ"])
        XCTAssertEqual(stub.calls.map(\.reading), ["あの", "きしゃ"])
    }

    func testLongestMatchWins() async throws {
        let (engine, _) = makeEngine([
            segment("きしゃ", "記者"),
            segment("きしゃに", "汽車に"),
        ])
        let result = try await engine.convert(reading: "きしゃにのる", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["汽車にノル"])
    }

    func testCandidatePanelPutsLearnedResultsFirst() async throws {
        let (engine, _) = makeEngine([segment("きしゃ", "記者")])
        let result = try await engine.convert(reading: "きしゃ", context: "", candidateCount: 3)
        XCTAssertEqual(result.first, "記者")
        XCTAssertTrue(result.contains("キシャ"), "エンジンの候補も残る")
    }

    func testCandidatePanelPrefersContextMatch() async throws {
        let (engine, _) = makeEngine([
            segment("きしゃ", "記者"),
            segment("きしゃ", "貴社", context: "記者の"),
        ])
        let result = try await engine.convert(
            reading: "きしゃ", context: "きょうは記者の", candidateCount: 3)
        XCTAssertEqual(result.first, "貴社", "文脈が合う学習結果を優先する")
        XCTAssertTrue(result.contains("記者"))
    }
}

final class LearningStoreTests: XCTestCase {

    private var url: URL!

    override func setUp() {
        super.setUp()
        url = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("iroha-learning-\(UUID().uuidString).json")
    }

    override func tearDown() {
        try? FileManager.default.removeItem(at: url)
        super.tearDown()
    }

    /// ユーザの例:「きしゃのきしゃ」を「記者の貴社」に直したら次から再現される
    func testRecordThenReproduce() async throws {
        let store = LearningStore(url: url)
        store.record(
            reading: "きしゃのきしゃ", result: "記者の貴社",
            segments: [(reading: "きしゃの", result: "記者の"), (reading: "きしゃ", result: "貴社")])

        // 入力全体が同じならそのまま
        XCTAssertEqual(store.current.sentence(forReading: "きしゃのきしゃ"), "記者の貴社")

        // 続きが違っても文節の学習で再現される
        let stub = StubEngine()
        let dictionary = store.current
        let engine = LearningEngine(base: stub, dictionary: { dictionary })
        let result = try await engine.convert(
            reading: "きしゃのきしゃがきた", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["記者の貴社ガキタ"])
    }

    func testLeftContextIsRecordedPerSegment() {
        let store = LearningStore(url: url)
        store.record(
            reading: "きしゃのきしゃ", result: "記者の貴社",
            segments: [(reading: "きしゃの", result: "記者の"), (reading: "きしゃ", result: "貴社")])
        let segments = store.current.entries.filter { $0.kind == .segment }
        XCTAssertEqual(segments.first { $0.result == "記者の" }?.leftContext, "")
        XCTAssertEqual(segments.first { $0.result == "貴社" }?.leftContext, "記者の")
    }

    func testLatestRecordWins() {
        let store = LearningStore(url: url)
        store.record(reading: "きしゃ", result: "記者", segments: [(reading: "きしゃ", result: "記者")])
        store.record(reading: "きしゃ", result: "汽車", segments: [(reading: "きしゃ", result: "汽車")])
        XCTAssertEqual(store.current.sentence(forReading: "きしゃ"), "汽車")
        XCTAssertEqual(store.current.learnedResults(forReading: "きしゃ", leftContext: "").first, "汽車")
    }

    func testPersistsAcrossInstances() {
        let store = LearningStore(url: url)
        store.record(
            reading: "きしゃのきしゃ", result: "記者の貴社",
            segments: [(reading: "きしゃの", result: "記者の"), (reading: "きしゃ", result: "貴社")])
        // 保存はバックグラウンドなので書き込みを待つ
        let deadline = Date().addingTimeInterval(2)
        while !FileManager.default.fileExists(atPath: url.path), Date() < deadline {
            RunLoop.current.run(until: Date().addingTimeInterval(0.01))
        }
        let reloaded = LearningStore(url: url)
        XCTAssertEqual(reloaded.current.sentence(forReading: "きしゃのきしゃ"), "記者の貴社")
        XCTAssertEqual(
            reloaded.current.learnedResults(forReading: "きしゃ", leftContext: "記者の").first, "貴社")
    }

    func testResetClearsEverything() {
        let store = LearningStore(url: url)
        store.record(reading: "きしゃ", result: "記者", segments: [(reading: "きしゃ", result: "記者")])
        store.reset()
        XCTAssertTrue(store.current.isEmpty)
        XCTAssertEqual(store.count, 0)
    }
}
