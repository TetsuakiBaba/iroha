import XCTest
@testable import IrohaCore

/// 呼び出しを記録するだけのダミーエンジン（既定では読みをカタカナにして返す）
private final class StubEngine: ConversionEngine, @unchecked Sendable {
    private(set) var calls: [(reading: String, context: String, count: Int)] = []
    var shouldFail = false
    /// 第一候補を読みと文脈から決める（左文脈で変換が変わるエンジンの代わり）
    var firstCandidate: (String, String) -> String = { reading, _ in hiraganaToKatakana(reading) }

    func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        calls.append((reading, context, candidateCount))
        if shouldFail { throw ConversionError.inferenceFailed("テスト") }
        let first = firstCandidate(reading, context)
        return (0..<max(1, candidateCount)).map { index in
            index == 0 ? first : "\(hiraganaToKatakana(reading))\(index)"
        }
    }
}

final class LearningEngineTests: XCTestCase {

    private func makeEngine(_ entries: [LearningEntry]) -> (LearningEngine, StubEngine) {
        let stub = StubEngine()
        let dictionary = LearningDictionary(entries: entries)
        return (LearningEngine(base: stub, dictionary: { dictionary }), stub)
    }

    /// 左文脈で変換が変わるエンジン: 「御恩と」の後だけ「奉公」、それ以外は「方向」
    private func makeContextEngine(_ entries: [LearningEntry]) -> (LearningEngine, StubEngine) {
        let (engine, stub) = makeEngine(entries)
        stub.firstCandidate = { reading, context in
            guard reading == "ほうこう" else { return hiraganaToKatakana(reading) }
            return context.hasSuffix("御恩と") ? "奉公" : "方向"
        }
        return (engine, stub)
    }

    func testEmptyLearningPassesThrough() async throws {
        let (engine, stub) = makeEngine([])
        let result = try await engine.convert(reading: "きしゃ", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["キシャ"])
        XCTAssertEqual(stub.calls.count, 1)
    }

    /// エンジンが直す前と同じ結果を出したら差し替える
    func testReplacesWhenEngineRepeatsCorrectedResult() async throws {
        let (engine, _) = makeContextEngine([
            LearningEntry(reading: "ほうこう", replaced: "方向", result: "芳香")
        ])
        let result = try await engine.convert(reading: "ほうこう", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["芳香"])
    }

    /// ユーザの例: 左文脈「御恩と」でエンジンが「奉公」を出したら、学習の「芳香」に差し替えない
    func testKeepsEngineResultWhenContextChangesIt() async throws {
        let (engine, stub) = makeContextEngine([
            LearningEntry(reading: "ほうこう", replaced: "方向", result: "芳香")
        ])
        let result = try await engine.convert(reading: "ほうこう", context: "御恩と", candidateCount: 1)
        XCTAssertEqual(result, ["奉公"])
        XCTAssertEqual(stub.calls.map(\.context), ["御恩と"], "文脈はそのままエンジンに渡す")
    }

    /// 差し替えなかったときも、候補ウィンドウでは学習結果を2番目に出す
    func testCandidatePanelPutsLearnedResultSecondWhenNotReplaced() async throws {
        let (engine, _) = makeContextEngine([
            LearningEntry(reading: "ほうこう", replaced: "方向", result: "芳香")
        ])
        let result = try await engine.convert(reading: "ほうこう", context: "御恩と", candidateCount: 3)
        XCTAssertEqual(Array(result.prefix(2)), ["奉公", "芳香"])
    }

    func testCandidatePanelPutsLearnedResultFirstWhenReplaced() async throws {
        let (engine, _) = makeContextEngine([
            LearningEntry(reading: "ほうこう", replaced: "方向", result: "芳香")
        ])
        let result = try await engine.convert(reading: "ほうこう", context: "", candidateCount: 3)
        XCTAssertEqual(result.first, "芳香")
        XCTAssertTrue(result.contains("方向"), "エンジンの候補も残る")
    }

    /// 「直す前」の記録がない学習（この条件を覚える前のもの）は、読みが一致すれば常に差し替える
    func testUnconditionalEntryAlwaysReplaces() async throws {
        let (engine, stub) = makeContextEngine([LearningEntry(reading: "ほうこう", result: "芳香")])
        let result = try await engine.convert(reading: "ほうこう", context: "御恩と", candidateCount: 1)
        XCTAssertEqual(result, ["芳香"])
        XCTAssertEqual(stub.calls.count, 1, "直す前の結果を記録できるよう、エンジンは呼ぶ")
        XCTAssertEqual(engine.engineResult(forReading: "ほうこう", shown: "芳香"), "奉公")
    }

    /// 同じ読みでも「直す前」ごとに別の学習として持てる
    func testEntriesPerEngineResult() async throws {
        let (engine, _) = makeContextEngine([
            LearningEntry(reading: "ほうこう", replaced: "方向", result: "芳香"),
            LearningEntry(reading: "ほうこう", replaced: "奉公", result: "放校"),
        ])
        let plain = try await engine.convert(reading: "ほうこう", context: "", candidateCount: 1)
        let afterGoon = try await engine.convert(reading: "ほうこう", context: "御恩と", candidateCount: 1)
        XCTAssertEqual(plain, ["芳香"])
        XCTAssertEqual(afterGoon, ["放校"])
    }

    /// 直近の第一候補の変換で、学習が差し替える前のエンジンの結果を引ける
    func testEngineResultOfRecentDecision() async throws {
        let (engine, _) = makeContextEngine([
            LearningEntry(reading: "ほうこう", replaced: "方向", result: "芳香")
        ])
        _ = try await engine.convert(reading: "ほうこう", context: "", candidateCount: 1)
        XCTAssertEqual(engine.engineResult(forReading: "ほうこう", shown: "芳香"), "方向")
        XCTAssertNil(engine.engineResult(forReading: "ほうこう", shown: "奉公"), "表示と違えば引かない")
        XCTAssertNil(engine.engineResult(forReading: "きしゃ", shown: "キシャ"), "学習の無い読みは記録しない")
    }

    /// 読み全体が一致しなければ学習は効かない（文中の一部には当てはめない）
    func testPartialReadingDoesNotMatch() async throws {
        let (engine, stub) = makeEngine([LearningEntry(reading: "きしゃ", result: "貴社")])
        let result = try await engine.convert(reading: "きしゃがきた", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["キシャガキタ"])
        XCTAssertEqual(stub.calls.map(\.reading), ["きしゃがきた"], "読みを分割しない")
    }

    /// 候補ウィンドウはエンジンが失敗しても学習結果だけで開く
    func testCandidatePanelSurvivesEngineFailure() async throws {
        let (engine, stub) = makeEngine([
            LearningEntry(reading: "きしゃ", replaced: "キシャ", result: "記者")
        ])
        stub.shouldFail = true
        let result = try await engine.convert(reading: "きしゃ", context: "", candidateCount: 3)
        XCTAssertEqual(result, ["記者"])
    }

    /// エンジンが失敗したとき、第一候補で返せるのは条件のない学習だけ
    func testFirstCandidateOnEngineFailure() async throws {
        let (conditional, stub1) = makeEngine([
            LearningEntry(reading: "きしゃ", replaced: "キシャ", result: "記者")
        ])
        stub1.shouldFail = true
        do {
            _ = try await conditional.convert(reading: "きしゃ", context: "", candidateCount: 1)
            XCTFail("直す前と比べられないので差し替えない")
        } catch {}

        let (unconditional, stub2) = makeEngine([LearningEntry(reading: "きしゃ", result: "記者")])
        stub2.shouldFail = true
        let result = try await unconditional.convert(reading: "きしゃ", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["記者"])
    }

    /// 同じ読み・同じ「直す前」のエントリが複数あれば新しい方を使う
    func testNewerEntryWins() async throws {
        let (engine, _) = makeEngine([
            LearningEntry(reading: "きしゃ", replaced: "キシャ", result: "記者",
                          updatedAt: Date(timeIntervalSince1970: 1)),
            LearningEntry(reading: "きしゃ", replaced: "キシャ", result: "汽車",
                          updatedAt: Date(timeIntervalSince1970: 2)),
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
        store.record(reading: "きしゃのきしゃ", replaced: "キシャノキシャ", result: "記者の貴社")

        XCTAssertEqual(
            store.current.entry(forReading: "きしゃのきしゃ", engineResult: "キシャノキシャ")?.result,
            "記者の貴社")

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
        store.record(reading: "きしゃのきしゃ", replaced: "キシャノキシャ", result: "記者の貴社")
        XCTAssertEqual(store.current.entries.count, 1)
        XCTAssertTrue(store.current.entries(forReading: "きしゃ").isEmpty)
    }

    /// 同じ読み・同じ「直す前」なら最新の修正で上書きする
    func testLatestRecordWins() {
        let store = LearningStore(url: url)
        store.record(reading: "きしゃ", replaced: "キシャ", result: "記者")
        store.record(reading: "きしゃ", replaced: "キシャ", result: "汽車")
        XCTAssertEqual(store.current.entries.count, 1)
        XCTAssertEqual(store.current.entry(forReading: "きしゃ", engineResult: "キシャ")?.result, "汽車")
    }

    /// 「直す前」が違えば別の学習として残す
    func testDifferentEngineResultsAreSeparateEntries() {
        let store = LearningStore(url: url)
        store.record(reading: "ほうこう", replaced: "方向", result: "芳香")
        store.record(reading: "ほうこう", replaced: "奉公", result: "放校")
        XCTAssertEqual(store.current.entries.count, 2)
        XCTAssertEqual(store.current.entry(forReading: "ほうこう", engineResult: "方向")?.result, "芳香")
        XCTAssertEqual(store.current.entry(forReading: "ほうこう", engineResult: "奉公")?.result, "放校")
        XCTAssertNil(store.current.entry(forReading: "ほうこう", engineResult: "砲口"))
    }

    /// 「直す前」を記録した修正は、同じ読みの記録なしの学習を置き換える
    func testConditionalRecordReplacesUnconditionalEntry() {
        let store = LearningStore(url: url)
        store.record(reading: "ほうこう", result: "芳香")
        store.record(reading: "ほうこう", replaced: "奉公", result: "放校")
        XCTAssertEqual(store.current.entries.map(\.result), ["放校"])
        XCTAssertNil(store.current.entry(forReading: "ほうこう", engineResult: "方向"))
    }

    /// 差し替えた結果をエンジンの結果に戻して確定したら、その学習を消す
    func testForgetRemovesAppliedEntry() {
        let store = LearningStore(url: url)
        store.record(reading: "ほうこう", replaced: "方向", result: "芳香")
        store.record(reading: "ほうこう", replaced: "奉公", result: "放校")
        store.forget(reading: "ほうこう", engineResult: "方向", result: "芳香")
        XCTAssertEqual(store.current.entries.map(\.result), ["放校"])
        // 結果が違えば消さない
        store.forget(reading: "ほうこう", engineResult: "奉公", result: "芳香")
        XCTAssertEqual(store.current.entries.count, 1)
    }

    /// 記録なしの古い学習も、戻して確定すれば消える
    func testForgetRemovesUnconditionalEntry() {
        let store = LearningStore(url: url)
        store.record(reading: "ほうこう", result: "芳香")
        store.forget(reading: "ほうこう", engineResult: "奉公", result: "芳香")
        XCTAssertTrue(store.current.isEmpty)
    }

    func testPersistsAcrossInstances() {
        let store = LearningStore(url: url)
        store.record(reading: "きしゃのきしゃ", replaced: "キシャノキシャ", result: "記者の貴社")
        // 保存はバックグラウンドなので書き込みを待つ
        let deadline = Date().addingTimeInterval(2)
        while !FileManager.default.fileExists(atPath: url.path), Date() < deadline {
            RunLoop.current.run(until: Date().addingTimeInterval(0.01))
        }
        let reloaded = LearningStore(url: url)
        let entry = reloaded.current.entries.first
        XCTAssertEqual(entry?.replaced, "キシャノキシャ")
        XCTAssertEqual(entry?.result, "記者の貴社")
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
        XCTAssertNil(store.current.entries.first?.replaced, "「直す前」の記録がない学習として読む")
        XCTAssertTrue(store.current.entries(forReading: "きしゃ").isEmpty)
    }

    func testResetClearsEverything() {
        let store = LearningStore(url: url)
        store.record(reading: "きしゃ", result: "記者")
        store.reset()
        XCTAssertTrue(store.current.isEmpty)
        XCTAssertEqual(store.count, 0)
    }
}
