import Foundation
import XCTest
@testable import IrohaCore

final class VoiceLogTests: XCTestCase {

    private var dir: URL!
    private var log: VoiceLog!

    override func setUpWithError() throws {
        dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-voicelog-\(UUID().uuidString)", isDirectory: true)
        log = VoiceLog(directory: dir.appendingPathComponent("voice"), hostName: "testhost")
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: dir)
    }

    /// 発話と確定を書いて読み戻すと、同じ内容が `id` でつながって返る
    func testRecordsUtteranceAndCommit() throws {
        let date = Date(timeIntervalSince1970: 1_795_000_000)  // 2026-11
        let transcript = VoiceTranscript(parts: [
            .reading("えーきょうは", surface: "えー今日は"), .fixed("iPhone"), .reading("の", surface: "の"),
        ])
        let utterance = VoiceLogEntry.utterance(
            id: "u1", recognizer: "speech", recognized: "えー今日は iPhoneの", transcript: transcript, timestamp: date)
        let commit = VoiceLogEntry.commit(utterances: ["u1"], committed: "今日はiPhoneの", timestamp: date)
        log.record(utterance)
        log.record(commit)
        log.waitUntilIdle()

        let url = log.fileURL(for: date)
        XCTAssertTrue(url.lastPathComponent.hasPrefix("voice-testhost-"))
        XCTAssertEqual(log.fileURLs(), [url])
        let entries = log.entries(in: url)
        XCTAssertEqual(entries, [utterance, commit])
        XCTAssertEqual(entries[0].surface, "えー今日はiPhoneの")
        XCTAssertEqual(entries[0].reading, "えーきょうはiPhoneの")
        XCTAssertEqual(entries[1].utterances, ["u1"])
        XCTAssertEqual(log.utteranceCount(), 1)
    }

    /// 発話の行には確定の項目を、確定の行には発話の項目を書かない
    func testOmitsFieldsOfOtherKind() throws {
        let date = Date(timeIntervalSince1970: 1_795_000_000)
        log.record(.commit(utterances: ["u1", "u2"], committed: "あした行きます", timestamp: date))
        log.waitUntilIdle()
        let line = try String(contentsOf: log.fileURL(for: date), encoding: .utf8)
        XCTAssertTrue(line.contains("\"kind\":\"commit\""))
        XCTAssertFalse(line.contains("recognized"))
        XCTAssertFalse(line.contains("surface"))
    }
}
