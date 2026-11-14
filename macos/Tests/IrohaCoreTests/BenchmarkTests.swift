import XCTest
@testable import IrohaCore

/// 変換のベンチマーク: 評価セットの読み込み・採点・集計（`testdata/speed/summarize.py` と同じ定義）・記録・取得
final class BenchmarkTests: XCTestCase {

    private var root: URL!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-bench-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        DataDirectory.configure(root.appendingPathComponent("data", isDirectory: true))
    }

    override func tearDownWithError() throws {
        DataDirectory.configure(DataDirectory.defaultURL)
        try? FileManager.default.removeItem(at: root)
    }

    /// 読みをそのまま返すか、表から引いて返すだけのエンジン
    private struct TableEngine: ConversionEngine {
        let table: [String: String]
        func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
            [table[reading] ?? reading]
        }
        func prewarm() async throws {}
    }

    // MARK: - 読み込み

    func testLoadsJSONArrayAndJSONLines() throws {
        let array = """
        [{"index": "a-1", "context_text": "", "input": "キョウ", "expected_output": ["今日"]},
         {"index": "a-2", "context_text": "前の", "input": "ハナシ", "expected_output": ["話", "はなし"]}]
        """
        let items = try Benchmark.loadItems(from: Data(array.utf8))
        XCTAssertEqual(items.map(\.index), ["a-1", "a-2"])
        XCTAssertNil(items[0].source)
        XCTAssertEqual(items[1].expectedOutput, ["話", "はなし"])

        // 配布用の JSON Lines（出典・ライセンスなど余分なキーがあり、空行・CRLF もありうる）
        let lines = """
        {"index": "b-1", "source": "jmrd", "context_text": "", "input": "キョウ", "expected_output": ["今日"], "license": "CC BY-SA 4.0"}\r

        {"index": "b-2", "source": "mrmp", "context_text": "x", "input": "ハナシ", "expected_output": ["話"]}
        """
        let parsed = try Benchmark.loadItems(from: Data(lines.utf8))
        XCTAssertEqual(parsed.map(\.index), ["b-1", "b-2"])
        XCTAssertEqual(parsed.map(\.source), ["jmrd", "mrmp"])
    }

    func testRejectsBrokenOrEmptySets() {
        XCTAssertThrowsError(try Benchmark.loadItems(from: Data("\n\n".utf8))) {
            XCTAssertEqual($0 as? Benchmark.LoadError, .empty)
        }
        let broken = "{\"index\": \"a\", \"context_text\": \"\", \"input\": \"ア\", \"expected_output\": [\"亜\"]}\n{oops}\n"
        XCTAssertThrowsError(try Benchmark.loadItems(from: Data(broken.utf8))) {
            XCTAssertEqual($0 as? Benchmark.LoadError, .invalidLine(2))
        }
    }

    // MARK: - 採点

    func testScoreUsesClosestReference() {
        let hit = Benchmark.score(output: "今日は", expected: ["きょうは", "今日は"])
        XCTAssertTrue(hit.hit)
        XCTAssertEqual(hit.minCER, 0)

        // 「京は」は「今日は」まで 2 文字（2/3）、「きょうは」まで 3 文字（3/4）。近いほうを取る
        let miss = Benchmark.score(output: "京は", expected: ["きょうは", "今日は"])
        XCTAssertFalse(miss.hit)
        XCTAssertEqual(miss.minCER, 2.0 / 3.0, accuracy: 1e-9)
    }

    func testEditDistance() {
        XCTAssertEqual(Benchmark.editDistance(Array("かきくけこ"), Array("かくけこさ")), 2)
        XCTAssertEqual(Benchmark.editDistance([], Array("あい")), 2)
        XCTAssertEqual(Benchmark.editDistance(Array("同じ"), Array("同じ")), 0)
    }

    // MARK: - 集計

    /// summarize.py の percentile（最近順位法）と同じ値になる
    func testPercentileMatchesSummarizeScript() {
        let values = (1...10).map(Double.init)
        XCTAssertEqual(Benchmark.Latency.percentile(values, 50), 5)
        XCTAssertEqual(Benchmark.Latency.percentile(values, 90), 9)
        XCTAssertEqual(Benchmark.Latency.percentile(values, 99), 10)
        XCTAssertEqual(Benchmark.Latency.percentile([7], 50), 7)
        let latency = Benchmark.Latency([3, 1, 2])
        XCTAssertEqual(latency.mean, 2)
        XCTAssertEqual(latency.p50, 2)
        XCTAssertEqual(latency.max, 3)
    }

    func testRunAndSummarize() async throws {
        let items = [
            Benchmark.Item(index: "1", source: "s1", contextText: "", input: "キョウ", expectedOutput: ["今日"]),
            Benchmark.Item(index: "2", source: "s1", contextText: "前", input: "ハナシ", expectedOutput: ["話"]),
            Benchmark.Item(index: "3", source: "s2", contextText: "",
                           input: String(repeating: "ア", count: 25), expectedOutput: ["亜"]),
        ]
        let engine = TableEngine(table: ["キョウ": "今日", "ハナシ": "放し"])
        var results: [Benchmark.ItemResult] = []
        for item in items { results.append(await Benchmark.run(item, engine: engine)) }
        XCTAssertEqual(results.map(\.hit), [true, false, false])
        XCTAssertEqual(results.map(\.readingLength), [3, 3, 25])

        let summary = Benchmark.Summary(results)
        XCTAssertEqual(summary.total.count, 3)
        XCTAssertEqual(summary.total.hits, 1)
        XCTAssertEqual(summary.withContext.count, 1)
        XCTAssertEqual(summary.withoutContext.hits, 1)
        XCTAssertEqual(summary.perSource.map(\.source), ["s1", "s2"])
        XCTAssertEqual(summary.perSource.first?.tally.hits, 1)
        // 1〜10 に 2 件、21〜30 に 1 件。項目の無い区切りは出さない
        XCTAssertEqual(summary.lengthBins.map(\.label), ["1〜10", "21〜30"])
        XCTAssertEqual(summary.lengthBins.map(\.count), [2, 1])

        let records = BenchmarkRun.Record.misses(items: items, results: results)
        XCTAssertEqual(records.map(\.index), ["2", "3"])
        XCTAssertEqual(records.first?.output, "放し")
    }

    func testTimingsTSVHasSummarizeColumns() {
        let item = Benchmark.Item(index: "x", contextText: "ab", input: "アイウ", expectedOutput: ["愛う"])
        var timing = InferenceTimer.Snapshot()
        timing.decode = .milliseconds(2)
        timing.decodeSteps = 3
        let result = Benchmark.ItemResult(item: item, output: "愛う", milliseconds: 4.5, timing: timing)
        let tsv = Benchmark.timingsTSV([result])
        let lines = tsv.split(separator: "\n")
        XCTAssertEqual(lines.first, "index\treading_length\tcontext_length\tms\tprompt_ms\tdecode_ms\tdecode_steps\tselect_ms")
        XCTAssertEqual(lines.last, "x\t3\t2\t4.500\t0.000\t2.000\t3\t0.000")
    }

    // MARK: - 記録と進み具合

    func testRecordRoundTripAndEvents() async throws {
        let item = Benchmark.Item(index: "1", contextText: "", input: "キョウ", expectedOutput: ["今日"])
        let result = await Benchmark.run(item, engine: TableEngine(table: [:]))
        let environment = BenchmarkRun.Environment(
            platform: "macos", appVersion: "1.0", hostName: "test", machineModel: "Mac", cpu: "Apple M", cpuCores: 8,
            osVersion: "27", powerSource: "ac", powerSaving: false,
            thermalStateBefore: "nominal", thermalStateAfter: "fair")
        let started = Date(timeIntervalSince1970: 1_800_000_000)
        let id = BenchmarkRun.makeID(date: started, hostName: "my mac.local")
        XCTAssertTrue(id.hasSuffix("-my_mac_local"))
        let record = BenchmarkRun.Record(
            id: id, benchmarkID: "set", benchmarkName: "Set",
            condition: .init(lattice: .always, backend: "llama", model: "m.gguf", adapter: nil,
                             device: "MTL0: Apple M", threads: 4),
            environment: environment, startedAt: started, finishedAt: started.addingTimeInterval(5),
            warmUpMilliseconds: 100, inputDuringRun: true, summary: Benchmark.Summary([result]),
            misses: BenchmarkRun.Record.misses(items: [item], results: [result]))
        try BenchmarkRun.write(record, results: [result])

        XCTAssertEqual(BenchmarkRun.records(), [record])
        let tsv = try String(contentsOf: BenchmarkRun.timingsURL(id: id), encoding: .utf8)
        XCTAssertTrue(tsv.hasPrefix(Benchmark.timingsHeader))
        XCTAssertEqual(record.formatVersion, BenchmarkRun.Record.currentFormatVersion)
        let markdown = BenchmarkRun.markdown(record)
        XCTAssertTrue(markdown.contains("| 全体 | 1 | 0 (0.0%) |"))
        XCTAssertTrue(markdown.contains("llama.cpp（MTL0: Apple M、4 スレッド）"))
        XCTAssertEqual(environment.throttlingNote, "発熱の状態 nominal → fair")
        BenchmarkRun.delete(id: id)
        XCTAssertTrue(BenchmarkRun.records().isEmpty)

        let event = BenchmarkRun.Event(kind: .progress, done: 3, total: 10, hits: 2)
        XCTAssertTrue(event.jsonLine().hasSuffix("\n"))
        XCTAssertEqual(BenchmarkRun.Event.parse(event.jsonLine().trimmingCharacters(in: .newlines)), event)
        XCTAssertNil(BenchmarkRun.Event.parse("llama.cpp のログ"))
    }

    /// 形式 1（最初の版。macOS だけの名前 chip・lowPowerMode）の記録も読める
    func testReadsFormatOneRecord() throws {
        let old = """
        {"benchmarkID": "iroha-bench-v1-quick", "benchmarkName": "iroha-bench-v1（抜粋）",
         "condition": {"backend": "llama", "lattice": "off", "model": "m.gguf"},
         "environment": {"appVersion": "0.17.1", "chip": "Apple M2", "hostName": "bell", "lowPowerMode": true,
                         "machineModel": "Mac14,2", "osVersion": "Version 27.0", "powerSource": "ac",
                         "thermalStateAfter": "nominal", "thermalStateBefore": "nominal"},
         "finishedAt": "2026-11-14T12:56:12Z", "id": "20261114-215532-bell", "inputDuringRun": false,
         "misses": [], "startedAt": "2026-11-14T12:55:32Z", "warmUpMilliseconds": 300,
         "summary": \(String(decoding: try BenchmarkRun.encoder.encode(Benchmark.Summary([])), as: UTF8.self))}
        """
        let record = try BenchmarkRun.decoder.decode(BenchmarkRun.Record.self, from: Data(old.utf8))
        XCTAssertNil(record.formatVersion)
        XCTAssertEqual(record.environment.platform, "macos")
        XCTAssertEqual(record.environment.cpu, "Apple M2")
        XCTAssertEqual(record.environment.powerSaving, true)
        XCTAssertNil(record.environment.cpuCores)
        XCTAssertNil(record.condition.device)
        XCTAssertEqual(record.inputDuringRun, false)
        XCTAssertEqual(record.environment.throttlingNote, "省電力の設定")

        // 書き直すと新しい名前だけになる
        let json = String(decoding: try BenchmarkRun.encoder.encode(record.environment), as: UTF8.self)
        XCTAssertTrue(json.contains("\"cpu\""))
        XCTAssertFalse(json.contains("chip"))
        XCTAssertFalse(json.contains("lowPowerMode"))
    }

    /// 取れない項目（Linux の発熱・Wayland のキー入力など）は書かない
    func testOmitsUnknownFields() throws {
        let environment = BenchmarkRun.Environment(
            platform: "linux", appVersion: "1.0", hostName: "gpu", machineModel: nil, cpu: "AMD EPYC", cpuCores: 64,
            osVersion: "Ubuntu 24.04", powerSource: nil, powerSaving: nil,
            thermalStateBefore: nil, thermalStateAfter: nil)
        let json = String(decoding: try BenchmarkRun.encoder.encode(environment), as: UTF8.self)
        XCTAssertFalse(json.contains("thermalState"))
        XCTAssertFalse(json.contains("powerSaving"))
        XCTAssertNil(environment.throttlingNote)
        XCTAssertEqual(try BenchmarkRun.decoder.decode(BenchmarkRun.Environment.self, from: Data(json.utf8)), environment)
    }

    // MARK: - 取得

    func testInstallChecksSizeAndChecksum() async throws {
        let body = Data("{\"index\": \"1\", \"context_text\": \"\", \"input\": \"ア\", \"expected_output\": [\"亜\"]}\n".utf8)
        let source = root.appendingPathComponent("set.jsonl")
        try body.write(to: source)
        let good = BenchmarkCatalog.Entry(
            id: "test-set", name: "テスト", summary: nil, items: 1,
            file: .init(url: source, bytes: body.count, sha256: SHA256.hex(body)),
            license: "CC0", sources: [.init(name: "出典", holder: "誰か", license: "CC0", url: "https://example.com")])
        let record = try await BenchmarkFetcher.install(good)
        XCTAssertEqual(record.items, 1)
        XCTAssertEqual(BenchmarkInstall.installedRecords().map(\.id), ["test-set"])
        XCTAssertEqual(try Benchmark.loadItems(contentsOf: BenchmarkInstall.fileURL(id: "test-set")).count, 1)

        let broken = BenchmarkCatalog.Entry(
            id: "broken", name: "壊れた", summary: nil, items: 1,
            file: .init(url: source, bytes: body.count, sha256: String(repeating: "0", count: 64)),
            license: "CC0", sources: [])
        do {
            try await BenchmarkFetcher.install(broken)
            XCTFail("チェックサムが違うのに入った")
        } catch {
            XCTAssertEqual(error as? BenchmarkFetcher.FetchError, .checksumMismatch)
        }
        XCTAssertEqual(BenchmarkInstall.installedRecords().map(\.id), ["test-set"])

        try BenchmarkInstall.remove(id: "test-set")
        XCTAssertTrue(BenchmarkInstall.installedRecords().isEmpty)
    }
}
