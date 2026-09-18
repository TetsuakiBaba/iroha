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

    func testEmptyLearningPassesThrough() async throws {
        let (engine, stub) = makeEngine([])
        let result = try await engine.convert(reading: "きしゃ", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["キシャ"])
        XCTAssertEqual(stub.calls.count, 1)
    }

    func testWholeReadingMatchSkipsEngine() async throws {
        let (engine, stub) = makeEngine([
            LearningEntry(reading: "きしゃのきしゃ", result: "記者の貴社")
        ])
        let result = try await engine.convert(reading: "きしゃのきしゃ", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["記者の貴社"])
        XCTAssertTrue(stub.calls.isEmpty, "入力全体が一致すればエンジンを呼ばない")
    }

    /// 読み全体が一致しなければ学習は効かない（文中の一部には当てはめない）
    func testPartialReadingDoesNotMatch() async throws {
        let (engine, stub) = makeEngine([LearningEntry(reading: "きしゃ", result: "貴社")])
        let result = try await engine.convert(reading: "きしゃがきた", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["キシャガキタ"])
        XCTAssertEqual(stub.calls.map(\.reading), ["きしゃがきた"], "読みを分割しない")
    }

    /// 文脈は学習の適用条件にしない（同じ読みなら文中でも同じ結果を返す）
    func testContextDoesNotAffectMatch() async throws {
        let (engine, stub) = makeEngine([LearningEntry(reading: "きしゃ", result: "貴社")])
        let result = try await engine.convert(
            reading: "きしゃ", context: "きょうは記者の", candidateCount: 1)
        XCTAssertEqual(result, ["貴社"])
        XCTAssertTrue(stub.calls.isEmpty)
    }

    func testCandidatePanelPutsLearnedResultFirst() async throws {
        let (engine, _) = makeEngine([LearningEntry(reading: "きしゃ", result: "記者")])
        let result = try await engine.convert(reading: "きしゃ", context: "", candidateCount: 3)
        XCTAssertEqual(result.first, "記者")
        XCTAssertTrue(result.contains("キシャ"), "エンジンの候補も残る")
    }

    /// 候補ウィンドウはエンジンが失敗しても学習結果だけで開く
    func testCandidatePanelSurvivesEngineFailure() async throws {
        let (engine, stub) = makeEngine([LearningEntry(reading: "きしゃ", result: "記者")])
        stub.shouldFail = true
        let result = try await engine.convert(reading: "きしゃ", context: "", candidateCount: 3)
        XCTAssertEqual(result, ["記者"])
    }

    /// 同じ読みのエントリが複数あれば新しい方を使う
    func testNewerEntryWins() async throws {
        let (engine, _) = makeEngine([
            LearningEntry(reading: "きしゃ", result: "記者", updatedAt: Date(timeIntervalSince1970: 1)),
            LearningEntry(reading: "きしゃ", result: "汽車", updatedAt: Date(timeIntervalSince1970: 2)),
        ])
        let result = try await engine.convert(reading: "きしゃ", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["汽車"])
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
        store.record(reading: "きしゃのきしゃ", result: "記者の貴社")

        XCTAssertEqual(store.current.result(forReading: "きしゃのきしゃ"), "記者の貴社")

        let stub = StubEngine()
        let dictionary = store.current
        let engine = LearningEngine(base: stub, dictionary: { dictionary })
        let result = try await engine.convert(
            reading: "きしゃのきしゃ", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["記者の貴社"])
    }

    /// 記録するのは読み全体の1件だけ（文節ごとには覚えない）
    func testRecordStoresSingleEntry() {
        let store = LearningStore(url: url)
        store.record(reading: "きしゃのきしゃ", result: "記者の貴社")
        XCTAssertEqual(store.current.entries.count, 1)
        XCTAssertNil(store.current.result(forReading: "きしゃ"))
    }

    func testLatestRecordWins() {
        let store = LearningStore(url: url)
        store.record(reading: "きしゃ", result: "記者")
        store.record(reading: "きしゃ", result: "汽車")
        XCTAssertEqual(store.current.entries.count, 1)
        XCTAssertEqual(store.current.result(forReading: "きしゃ"), "汽車")
    }

    func testPersistsAcrossInstances() {
        let store = LearningStore(url: url)
        store.record(reading: "きしゃのきしゃ", result: "記者の貴社")
        // 保存はバックグラウンドなので書き込みを待つ
        let deadline = Date().addingTimeInterval(2)
        while !FileManager.default.fileExists(atPath: url.path), Date() < deadline {
            RunLoop.current.run(until: Date().addingTimeInterval(0.01))
        }
        let reloaded = LearningStore(url: url)
        XCTAssertEqual(reloaded.current.result(forReading: "きしゃのきしゃ"), "記者の貴社")
    }

    /// 文節の学習があった頃のファイルも読める（文節のエントリは捨てる）
    func testLegacySegmentEntriesAreDropped() throws {
        let legacy = """
        {"version":1,"entries":[
          {"kind":"sentence","leftContext":"","reading":"きしゃのきしゃ","result":"記者の貴社",
           "updatedAt":"2026-09-18T00:00:00Z"},
          {"kind":"segment","leftContext":"","reading":"きしゃの","result":"記者の",
           "updatedAt":"2026-09-18T00:00:00Z"},
          {"kind":"segment","leftContext":"記者の","reading":"きしゃ","result":"貴社",
           "updatedAt":"2026-09-18T00:00:00Z"}
        ]}
        """
        try Data(legacy.utf8).write(to: url)
        let store = LearningStore(url: url)
        XCTAssertEqual(store.current.entries.map(\.result), ["記者の貴社"])
        XCTAssertNil(store.current.result(forReading: "きしゃ"))
    }

    func testResetClearsEverything() {
        let store = LearningStore(url: url)
        store.record(reading: "きしゃ", result: "記者")
        store.reset()
        XCTAssertTrue(store.current.isEmpty)
        XCTAssertEqual(store.count, 0)
    }
}
