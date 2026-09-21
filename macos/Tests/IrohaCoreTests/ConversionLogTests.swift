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

    /// 読みをそのまま確定した1文字（「、」「の」など）は学習例にならないので isTrivial
    func testTrivialSingleCharacterCommits() throws {
        XCTAssertTrue(entry(reading: "、", proposed: "、", committed: "、").isTrivial)
        XCTAssertTrue(entry(reading: "の", proposed: "の", committed: "の").isTrivial)
        // 1文字でも変換が起きていれば中身がある
        XCTAssertFalse(entry(reading: "ぺーじ", proposed: "頁", committed: "頁").isTrivial)
        // モデルは漢字を出したのにユーザがかな1文字にした＝NNの誤りなので残す
        XCTAssertFalse(entry(reading: "の", proposed: "乃", committed: "の").isTrivial)
        // 提示が不明（edited == nil）なら判断できないので残す
        XCTAssertFalse(entry(reading: "の", proposed: nil, committed: "の").isTrivial)
        // 2文字以上のかな確定は「あえて漢字にしない」例なので残す
        XCTAssertFalse(entry(reading: "こと", proposed: "こと", committed: "こと").isTrivial)
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

    // MARK: - 編集（設定画面から）

    /// 編集テスト用の短い作り方（同じ月のファイルに入るよう timestamp は近い値にする）
    private func simple(_ reading: String, _ committed: String, proposed: String? = nil,
                        at seconds: TimeInterval = 0) -> ConversionLogEntry {
        entry(timestamp: Date(timeIntervalSince1970: 1_789_000_000 + seconds), context: "文脈",
              reading: reading, proposed: proposed, committed: committed)
    }

    /// records() は時系列順にファイル・行番号つきで返す
    func testRecordsReturnsFileAndLine() throws {
        log.record(simple("あ", "亜", at: 0))
        log.record(simple("い", "医", at: 1))
        log.waitUntilIdle()

        let records = log.records()
        XCTAssertEqual(records.count, 2)
        XCTAssertEqual(records.map(\.line), [0, 1])
        XCTAssertEqual(records.map(\.entry.committed), ["亜", "医"])
        XCTAssertEqual(Set(records.map(\.id)).count, 2)
    }

    /// データフォルダを共有していると他のMacのファイルが混ざるので、
    /// ファイル名順（＝ホスト名順）ではなく記録の時刻で並べる
    func testRecordsMergesHostFilesByTimestamp() throws {
        // 名前が後ろのホストの記録のほうが古い（ファイル名順に並べると時系列が壊れる組み合わせ）
        let other = ConversionLog(directory: log.directory, hostName: "zzz")
        other.record(simple("ふるい", "古い", at: 10))
        other.record(simple("ふるい2", "古い2", at: 20))
        other.waitUntilIdle()
        log.record(simple("あたらしい", "新しい", at: 30))
        log.waitUntilIdle()

        XCTAssertEqual(log.fileURLs().count, 2)
        XCTAssertEqual(log.records().map(\.entry.committed), ["古い", "古い2", "新しい"])
        // 画面は reversed() で最新が先頭に来る
        XCTAssertEqual(log.records().reversed().first?.entry.committed, "新しい")
        // 行番号は各ファイルの中の位置のまま（書き戻しの照合に使う）
        XCTAssertEqual(log.records().map(\.line), [0, 1, 0])
    }

    /// 1件を書き換えると、その行だけが変わる
    func testReplaceUpdatesSingleLine() throws {
        log.record(simple("あ", "亜", at: 0))
        log.record(simple("い", "位", proposed: "医", at: 1))
        log.record(simple("う", "雨", at: 2))
        log.waitUntilIdle()

        var target = log.records()[1]
        target.entry.committed = "医"
        target.entry.edited = false
        log.replace(target, with: target.entry)

        let records = log.records()
        XCTAssertEqual(records.map(\.entry.committed), ["亜", "医", "雨"])
        XCTAssertEqual(records[1].entry.edited, false)
        XCTAssertEqual(records[1].entry.reading, "い")
    }

    /// nil を渡すと削除。行番号がずれても内容で探し当てる
    func testReplaceWithNilDeletes() throws {
        log.record(simple("あ", "亜", at: 0))
        log.record(simple("い", "医", at: 1))
        log.waitUntilIdle()

        log.replace(log.records()[0], with: nil)
        XCTAssertEqual(log.records().map(\.entry.committed), ["医"])

        // 取得済みの古いスナップショット（行番号1）でも、内容が一致すれば消せる
        let stale = ConversionLog.Record(file: log.fileURL(for: Date(timeIntervalSince1970: 1_789_000_001)),
                                         line: 1, entry: simple("い", "医", at: 1))
        log.replace(stale, with: nil)
        XCTAssertTrue(log.records().isEmpty)
    }

    /// まとめて削除（ファイルをまたいでも1回ずつ書き戻す）
    func testDeleteMultipleAcrossMonths() throws {
        let september = Date(timeIntervalSince1970: 1_757_000_000)  // 2025-09
        let october = Date(timeIntervalSince1970: 1_759_700_000)    // 2025-10

        log.record(ConversionLogEntry(timestamp: september, mode: .live, context: "文脈", contextSource: .document,
                                      reading: "あ", proposed: nil, committed: "亜", model: "test"))
        log.record(ConversionLogEntry(timestamp: september.addingTimeInterval(60), mode: .live, context: "文脈",
                                      contextSource: .document, reading: "い", proposed: nil, committed: "医",
                                      model: "test"))
        log.record(ConversionLogEntry(timestamp: october, mode: .live, context: "文脈", contextSource: .document,
                                      reading: "う", proposed: nil, committed: "雨", model: "test"))
        log.waitUntilIdle()
        XCTAssertEqual(log.records().count, 3)

        let records = log.records()
        log.delete([records[0], records[2]])
        XCTAssertEqual(log.records().map(\.entry.committed), ["医"])
    }

    /// 全部消すとファイル自体を残さない
    func testDeletingLastEntryRemovesFile() throws {
        log.record(simple("あ", "亜", at: 0))
        log.waitUntilIdle()
        let url = log.fileURLs()[0]
        log.replace(log.records()[0], with: nil)
        XCTAssertFalse(FileManager.default.fileExists(atPath: url.path))
    }
}
