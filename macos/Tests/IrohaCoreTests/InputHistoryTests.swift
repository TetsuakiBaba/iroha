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
