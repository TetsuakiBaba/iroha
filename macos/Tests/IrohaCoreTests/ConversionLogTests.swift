import Foundation
import XCTest
@testable import IrohaCore

final class ConversionLogTests: XCTestCase {

    private var dir: URL!
    private var log: ConversionLog!

    override func setUpWithError() throws {
        dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-conversionlog-\(UUID().uuidString)", isDirectory: true)
        log = ConversionLog(directory: dir.appendingPathComponent("conversions"), hostName: "testhost")
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: dir)
    }

    private func entry(
        timestamp: Date = Date(timeIntervalSince1970: 1_789_000_000),  // 2026-09
        mode: ConversionLogEntry.Mode = .live, context: String = "本日は", reading: String = "きしゃに",
        proposed: String? = "貴社に", committed: String = "貴社に",
        segments: [ConversionLogEntry.Segment]? = nil
    ) -> ConversionLogEntry {
        ConversionLogEntry(
            timestamp: timestamp, mode: mode, context: context, contextSource: .document,
            reading: reading, proposed: proposed, committed: committed, segments: segments, model: "zenz-test")
    }

    /// 1行1件のJSONで追記され、読み戻せる。月ごとのファイルに分かれる
    func testAppendsOneJSONPerLineAndReadsBack() throws {
        let first = entry()
        let second = entry(
            mode: .segments, reading: "きしゃのきしゃ", proposed: "記者の汽車", committed: "記者の貴社",
            segments: [.init(reading: "きしゃの", result: "記者の"), .init(reading: "きしゃ", result: "貴社")])
        log.record(first)
        log.record(second)
        log.waitUntilIdle()

        let files = log.fileURLs()
        XCTAssertEqual(files.count, 1)
        XCTAssertEqual(files[0].lastPathComponent, "conversions-testhost-2026-09.jsonl")

        let text = try String(contentsOf: files[0], encoding: .utf8)
        let lines = text.split(separator: "\n")
        XCTAssertEqual(lines.count, 2)
        XCTAssertTrue(text.hasSuffix("\n"))
        // 1行が1つのJSONオブジェクト
        for line in lines {
            XCTAssertNotNil(try JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any])
        }

        let read = log.entries(in: files[0])
        XCTAssertEqual(read, [first, second])
        XCTAssertEqual(read[0].edited, false)
        XCTAssertEqual(read[1].edited, true)
        XCTAssertEqual(read[1].segments?.count, 2)
        XCTAssertEqual(read[0].format, ConversionLog.currentFormat)
        XCTAssertEqual(log.entryCount(), 2)
    }

    /// 別の月の確定は別のファイルに入る
    func testSplitsFilesByMonth() throws {
        log.record(entry(timestamp: Date(timeIntervalSince1970: 1_789_000_000)))  // 2026-09
        log.record(entry(timestamp: Date(timeIntervalSince1970: 1_792_000_000)))  // 2026-10
        log.waitUntilIdle()
        XCTAssertEqual(
            log.fileURLs().map(\.lastPathComponent),
            ["conversions-testhost-2026-09.jsonl", "conversions-testhost-2026-10.jsonl"])
    }

    /// 提示が不明（nil）なら edited も nil で、JSONにキーが出ない
    func testUnknownProposalOmitsEdited() throws {
        let unknown = entry(proposed: nil)
        XCTAssertNil(unknown.edited)
        let json = String(data: try ConversionLog.makeEncoder().encode(unknown), encoding: .utf8)!
        XCTAssertFalse(json.contains("\"edited\""))
        XCTAssertFalse(json.contains("\"proposed\""))
        XCTAssertFalse(json.contains("\"segments\""))
        XCTAssertTrue(json.contains("\"contextSource\":\"document\""))
        XCTAssertTrue(json.contains("\"mode\":\"live\""))
    }

    /// 学習用の行は prepare_data.py と同じ形（文脈40文字・カタカナ読み）。文脈が空ならタグごと省く
    func testTrainingLine() throws {
        let withContext = entry(context: "本日は", reading: "きしゃに", committed: "貴社に")
        XCTAssertEqual(withContext.trainingLine, "\u{EE02}本日は\u{EE00}キシャニ\u{EE01}貴社に")

        let long = entry(context: String(repeating: "あ", count: 50) + "本日は")
        XCTAssertEqual(long.trainingLine.count, 1 + 40 + 1 + 4 + 1 + 3)
        XCTAssertTrue(long.trainingLine.hasPrefix("\u{EE02}" + String(repeating: "あ", count: 37) + "本日は"))

        let noContext = entry(context: "")
        XCTAssertEqual(noContext.trainingLine, "\u{EE00}キシャニ\u{EE01}貴社に")
    }

    /// 合計サイズと削除
    func testTotalSizeAndRemoveAll() throws {
        XCTAssertEqual(log.totalSize(), 0)
        XCTAssertEqual(log.fileURLs(), [])
        log.record(entry())
        log.waitUntilIdle()
        XCTAssertGreaterThan(log.totalSize(), 0)

        log.removeAll()
        log.waitUntilIdle()
        XCTAssertEqual(log.totalSize(), 0)
        XCTAssertEqual(log.fileURLs(), [])
        XCTAssertEqual(log.entryCount(), 0)
    }

    /// 壊れた行は読み飛ばす
    func testSkipsCorruptLines() throws {
        log.record(entry())
        log.waitUntilIdle()
        let url = log.fileURLs()[0]
        let handle = try FileHandle(forWritingTo: url)
        _ = try handle.seekToEnd()
        try handle.write(contentsOf: Data("{not json\n".utf8))
        try handle.close()
        XCTAssertEqual(log.entries(in: url).count, 1)
        XCTAssertEqual(log.entryCount(), 2)  // 行数は2
    }
}
