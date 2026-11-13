import XCTest
@testable import IrohaCore

final class InputHistoryUnitsTests: XCTestCase {

    private func units(_ reading: String, _ text: String) -> [String] {
        InputHistoryUnits.units(reading: reading, text: text).map { "\($0.reading)→\($0.text)" }
    }

    func testKeepsSegmentAndWordPart() {
        XCTAssertEqual(units("もでるを", "モデルを"), ["もでるを→モデルを", "もでる→モデル"])
        XCTAssertEqual(units("かのうなのかな？", "可能なのかな？"), ["かのうなのかな→可能なのかな", "かのう→可能"])
    }

    func testSplitsLongCommitIntoSegments() {
        XCTAssertEqual(
            units("きょうはいいてんきですね", "今日はいい天気ですね"),
            ["きょうはいい→今日はいい", "きょう→今日", "てんきですね→天気ですね", "てんき→天気"])
    }

    func testKeepsHiraganaWord() {
        XCTAssertEqual(units("ちょっと", "ちょっと"), ["ちょっと→ちょっと"])
    }

    func testDropsShortAndNonRomajiPieces() {
        // 1文字の読みは候補になりえない
        XCTAssertEqual(units("の", "の"), [])
        // Shift+英字の英字（読みにひらがな以外）、F10 の英数（表記に英数字）は残さない
        XCTAssertEqual(units("macos", "macOS"), [])
        XCTAssertEqual(units("もでる", "moderu"), [])
    }
}

final class InputHistoryStoreTests: XCTestCase {

    private var directory: URL!
    private var stores: [InputHistoryStore] = []

    override func setUp() {
        super.setUp()
        directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-input-history-\(UUID().uuidString)", isDirectory: true)
    }

    override func tearDown() {
        // 保存を待ってから消す（後から書かれてフォルダが残る・保存に失敗するのを避ける）
        stores.forEach { $0.flush() }
        stores = []
        try? FileManager.default.removeItem(at: directory)
        super.tearDown()
    }

    private func makeStore(host: String) -> InputHistoryStore {
        let store = InputHistoryStore(directory: directory, hostName: host, saveDelay: 0)
        stores.append(store)
        return store
    }

    func testCandidatesRankedByCountThenRecency() {
        let store = makeStore(host: "a")
        let t0 = Date(timeIntervalSince1970: 1_000)
        store.record(reading: "もでる", text: "モデル", at: t0)
        store.record(reading: "もでる", text: "モデル", at: t0)
        store.record(reading: "もでむ", text: "モデム", at: t0.addingTimeInterval(10))
        store.record(reading: "もでらと", text: "モデラート", at: t0.addingTimeInterval(20))
        store.record(reading: "もでるが", text: "モデルが", at: t0.addingTimeInterval(5))
        let texts = store.candidates(prefix: "もで").map(\.text)
        // モデル(2回) → モデルが(1回) は「モデル」の語の部分も数えるので モデル は 3 回
        XCTAssertEqual(texts.first, "モデル")
        XCTAssertEqual(texts.count, InputHistoryStore.candidateLimit)
        XCTAssertEqual(Array(texts.dropFirst()), ["モデラート", "モデム"])
    }

    func testCandidateMustBeLongerThanPrefix() {
        let store = makeStore(host: "a")
        store.record(reading: "もでる", text: "モデル")
        XCTAssertEqual(store.candidates(prefix: "もでる"), [])
        XCTAssertEqual(store.candidates(prefix: "もでx"), [])
    }

    /// 決まった並びの乱数（テストを毎回同じにする）
    private struct Generator {
        var state: UInt64
        mutating func next(_ bound: Int) -> Int {
            state = state &* 6364136223846793005 &+ 1442695040888963407
            return Int((state >> 33) % UInt64(bound))
        }
    }

    private func randomReading(_ generator: inout Generator) -> String {
        let kana = Array("あいうかきくさしすたちつ")
        return String((0..<(2 + generator.next(4))).map { _ in kana[generator.next(kana.count)] })
    }

    /// 索引で引いた候補が、全件を先頭から調べたときと同じになる
    func testCandidatesMatchScanOfAllEntries() {
        let store = makeStore(host: "a")
        var generator = Generator(state: 1)
        let t0 = Date(timeIntervalSince1970: 1_000)
        for i in 0..<400 {
            let reading = randomReading(&generator)
            store.record(reading: reading, text: reading, at: t0.addingTimeInterval(Double(i)))
        }
        let entries = store.entries
        XCTAssertGreaterThan(entries.count, 100)
        func scan(_ prefix: String) -> [InputHistoryEntry] {
            var seen: Set<String> = []
            return entries
                .filter { $0.reading.hasPrefix(prefix) && $0.reading.count > prefix.count }
                .sorted {
                    if $0.count != $1.count { return $0.count > $1.count }
                    if $0.lastUsed != $1.lastUsed { return $0.lastUsed > $1.lastUsed }
                    return $0.reading < $1.reading
                }
                .filter { seen.insert($0.text).inserted }
                .prefix(InputHistoryStore.candidateLimit).map { $0 }
        }
        var nonEmpty = 0
        for prefix in ["あ", "か", "し", "つ", "あい", "かき", "しす", "たち", "あいう", "きくさ", "ん", "あx"] {
            let candidates = store.candidates(prefix: prefix)
            XCTAssertEqual(candidates, scan(prefix), prefix)
            if !candidates.isEmpty { nonEmpty += 1 }
        }
        XCTAssertGreaterThan(nonEmpty, 6)
        XCTAssertTrue(store.isConsistentWithFullMerge())
    }

    /// 記録のたびに変わった語だけ直した合算が、全件を合算し直した結果と同じになる
    /// （他のMacの分・消した記録・すべて消した日時が混ざっても）
    func testIncrementalMergeMatchesFullMerge() {
        let a = makeStore(host: "a")
        let b = makeStore(host: "b")
        var generator = Generator(state: 7)
        var now = Date(timeIntervalSince1970: 1_000)
        func check(_ step: String) {
            XCTAssertTrue(a.isConsistentWithFullMerge(), "a: \(step)")
            XCTAssertTrue(b.isConsistentWithFullMerge(), "b: \(step)")
        }
        for round in 0..<12 {
            for _ in 0..<20 {
                now += 1
                let reading = randomReading(&generator)
                (generator.next(2) == 0 ? a : b).record(reading: reading, text: reading, at: now)
            }
            check("記録 \(round)")
            a.flush(); b.flush()
            a.reloadIfChanged(); b.reloadIfChanged()
            check("読み直し \(round)")
            // b で消す → a に残っている分は数えなくなる → a でまた確定すると a の回数がまるごと戻る
            if round % 3 == 1, let entry = b.entries.max(by: { $0.count < $1.count }) {
                now += 1
                b.remove([entry.key], at: now)
                b.flush()
                a.reloadIfChanged()
                check("削除 \(round)")
                XCTAssertEqual(a.candidates(prefix: String(entry.reading.prefix(1))).filter { $0.key == entry.key }, [])
                now += 1
                a.record(reading: entry.reading, text: entry.text, at: now)
                check("削除のあとの記録 \(round)")
            }
            if round == 8 {
                now += 1
                b.reset(at: now)
                b.flush()
                a.reloadIfChanged()
                check("すべて消す")
            }
        }
    }

    /// 上限を超えたら最後に使った日時が古い語から捨て、合算と索引からも外す
    func testTrimDropsOldestAndUpdatesIndex() throws {
        let t0 = Date(timeIntervalSince1970: 1_000)
        var file = InputHistoryStore.HostFile()
        file.entries = (0..<InputHistoryStore.maxEntries).map {
            InputHistoryEntry(reading: "ごい\($0)", text: "語彙\($0)", count: 1, lastUsed: t0.addingTimeInterval(Double($0)))
        }
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try encoder.encode(file).write(to: directory.appendingPathComponent("a.json"))
        let store = makeStore(host: "a")
        XCTAssertEqual(store.count, InputHistoryStore.maxEntries)
        store.record(reading: "もでる", text: "モデル", at: t0.addingTimeInterval(100_000))
        XCTAssertEqual(store.count, InputHistoryStore.maxEntries)
        XCTAssertEqual(store.candidates(prefix: "もで").map(\.text), ["モデル"])
        XCTAssertFalse(store.entries.contains { $0.text == "語彙0" })
        XCTAssertTrue(store.entries.contains { $0.text == "語彙1" })
        XCTAssertTrue(store.isConsistentWithFullMerge())
        // いちばん古い日時で記録した語は、その場で捨てられる
        store.record(reading: "ふるい", text: "古い", at: t0)
        XCTAssertEqual(store.candidates(prefix: "ふる"), [])
        XCTAssertTrue(store.isConsistentWithFullMerge())
    }

    func testMergesHostsAndPersists() {
        let a = makeStore(host: "a")
        a.record(reading: "もでる", text: "モデル")
        a.flush()
        let b = makeStore(host: "b")
        b.record(reading: "もでる", text: "モデル")
        b.flush()
        XCTAssertEqual(b.candidates(prefix: "もで").first?.count, 2)
        XCTAssertTrue(a.reloadIfChanged())
        XCTAssertEqual(a.candidates(prefix: "もで").first?.count, 2)
        // 自分のファイルには自分の分だけ
        let reopened = makeStore(host: "a")
        XCTAssertEqual(reopened.count, 1)
        XCTAssertEqual(reopened.candidates(prefix: "もで").first?.count, 2)
    }

    func testRemoveHidesOtherHostsEarlierUse() {
        let t0 = Date(timeIntervalSince1970: 1_000)
        let a = makeStore(host: "a")
        a.record(reading: "もでる", text: "モデル", at: t0)
        a.flush()
        let b = makeStore(host: "b")
        b.record(reading: "もでる", text: "モデル", at: t0)
        b.remove([InputHistoryEntry.Key(reading: "もでる", text: "モデル")], at: t0.addingTimeInterval(1))
        b.flush()
        XCTAssertEqual(b.candidates(prefix: "もで"), [])
        a.reloadIfChanged()
        XCTAssertEqual(a.candidates(prefix: "もで"), [])
        // 消したあとにまた確定すれば戻る
        a.record(reading: "もでる", text: "モデル", at: t0.addingTimeInterval(2))
        XCTAssertEqual(a.candidates(prefix: "もで").map(\.text), ["モデル"])
    }

    func testResetHidesEverythingBefore() {
        let t0 = Date(timeIntervalSince1970: 1_000)
        let a = makeStore(host: "a")
        a.record(reading: "もでる", text: "モデル", at: t0)
        a.flush()
        let b = makeStore(host: "b")
        b.reset(at: t0.addingTimeInterval(1))
        b.flush()
        XCTAssertEqual(b.count, 0)
        a.reloadIfChanged()
        XCTAssertEqual(a.count, 0)
    }

    func testImportsOwnConversionLogOnce() throws {
        let logDirectory = directory.appendingPathComponent("conversions", isDirectory: true)
        let mine = ConversionLog(directory: logDirectory, hostName: "a")
        let other = ConversionLog(directory: logDirectory, hostName: "b")
        mine.record(ConversionLogEntry(
            mode: .live, context: "これは", contextSource: .document, reading: "もでるを",
            proposed: "モデルを", committed: "モデルを", segments: nil, model: "test"))
        other.record(ConversionLogEntry(
            mode: .live, context: "これは", contextSource: .document, reading: "でーたを",
            proposed: "データを", committed: "データを", segments: nil, model: "test"))
        mine.waitUntilIdle()
        other.waitUntilIdle()
        let store = InputHistoryStore(
            directory: directory.appendingPathComponent("history"), hostName: "a", saveDelay: 0)
        stores.append(store)
        XCTAssertEqual(store.importConversionLog(mine), 1)
        XCTAssertEqual(store.candidates(prefix: "もで").map(\.text), ["モデル", "モデルを"])
        XCTAssertEqual(store.candidates(prefix: "でー"), [])
        XCTAssertEqual(store.importConversionLog(mine), 0)
        XCTAssertEqual(store.candidates(prefix: "もで").first?.count, 1)
    }
}
