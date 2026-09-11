import Foundation
import XCTest
@testable import IrohaCore

final class LaunchLogTests: XCTestCase {

    private var dir: URL!
    private var log: LaunchLog!

    override func setUpWithError() throws {
        dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-launchlog-\(UUID().uuidString)", isDirectory: true)
        log = LaunchLog(directory: dir.appendingPathComponent("logs"), hostName: "testhost", maxLines: 10)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: dir)
    }

    /// 初回起動は launch の1行だけ。記録が書かれ、正常終了で終了時刻が入る
    func testLaunchAndCleanExit() throws {
        let lines = log.recordLaunch(
            version: "1.0", osVersion: "macOS 26", pid: 100, crashReportsDirectory: nil)
        XCTAssertEqual(lines.count, 1)
        XCTAssertTrue(lines[0].hasPrefix("launch pid=100 version=1.0 macOS=macOS 26 host=testhost"))
        XCTAssertEqual(log.readState()?.pid, 100)
        XCTAssertEqual(log.readState()?.version, "1.0")

        XCTAssertTrue(log.readState()!.isRunning)

        log.recordExit(reason: "terminate", pid: 100)
        let state = try XCTUnwrap(log.readState())
        XCTAssertFalse(state.isRunning)
        XCTAssertEqual(state.exitReason, "terminate")
        let written = log.lines()
        XCTAssertEqual(written.count, 2)
        XCTAssertTrue(written[1].contains("\texit reason=terminate pid=100"))
        // 行頭はタイムスタンプ
        XCTAssertNotNil(written[0].range(of: #"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\t"#, options: .regularExpression))
    }

    /// 前回の記録に終了が無く、そのPIDが死んでいれば unclean-exit
    func testUncleanExitDetected() throws {
        log.recordLaunch(version: "1.0", osVersion: "os", pid: 100, crashReportsDirectory: nil)
        let lines = log.recordLaunch(
            version: "1.1", osVersion: "os", pid: 200, crashReportsDirectory: nil, isProcessAlive: { _ in false })
        XCTAssertEqual(lines.count, 2)
        XCTAssertTrue(lines[0].hasPrefix("unclean-exit"))
        XCTAssertTrue(lines[0].contains("pid=100"))
        XCTAssertTrue(lines[0].contains("version=1.0"))
        XCTAssertTrue(lines[1].hasPrefix("launch pid=200"))
        XCTAssertEqual(log.readState()?.pid, 200)
    }

    /// 前回記録のPIDがまだ動いていれば別インスタンスとみなし、unclean-exit にはしない
    func testOtherInstanceRunningIsNote() throws {
        log.recordLaunch(version: "1.0", osVersion: "os", pid: 100, crashReportsDirectory: nil)
        let lines = log.recordLaunch(
            version: "1.0", osVersion: "os", pid: 200, crashReportsDirectory: nil, isProcessAlive: { $0 == 100 })
        XCTAssertTrue(lines[0].hasPrefix("note 別のirohaが動作中 pid=100"))
    }

    /// 他プロセスの記録には終了を書かない
    func testExitDoesNotTouchForeignState() throws {
        log.recordLaunch(version: "1.0", osVersion: "os", pid: 100, crashReportsDirectory: nil)
        log.recordExit(reason: "terminate", pid: 999)
        XCTAssertEqual(log.readState()?.pid, 100)
        XCTAssertTrue(log.readState()!.isRunning)
    }

    /// 正常終了した直前の記録は unclean-exit にならない
    func testCleanPreviousRunIsNotUnclean() throws {
        log.recordLaunch(version: "1.0", osVersion: "os", pid: 100, crashReportsDirectory: nil)
        log.recordExit(reason: "terminate", pid: 100)
        let lines = log.recordLaunch(
            version: "1.0", osVersion: "os", pid: 200, crashReportsDirectory: nil, isProcessAlive: { _ in false })
        XCTAssertEqual(lines.count, 1)
        XCTAssertTrue(lines[0].hasPrefix("launch pid=200"))
    }

    /// 上限を超えたら新しい半分だけ残す
    func testTrimKeepsNewestHalf() throws {
        for i in 0..<12 {
            log.recordExit(reason: "r\(i)", pid: 1)
        }
        let lines = log.lines()
        XCTAssertEqual(lines.count, 6)  // 11行目で上限10を超えて5行に切り、12行目を足して6行
        XCTAssertTrue(lines.first!.contains("reason=r6"))
        XCTAssertTrue(lines.last!.contains("reason=r11"))
    }

    /// クラッシュレポートの .ips を読んで要約する
    func testCrashReportParse() throws {
        let reports = dir.appendingPathComponent("reports")
        try FileManager.default.createDirectory(at: reports, withIntermediateDirectories: true)
        let file = reports.appendingPathComponent("iroha-2026-09-09-062759.ips")
        try Self.sampleIPS.write(to: file, atomically: true, encoding: .utf8)
        // 名前は一致するがヘッダのapp_nameが違うもの（iroha-cli）は除外
        try Self.sampleIPS.replacingOccurrences(of: "\"app_name\":\"iroha\"", with: "\"app_name\":\"iroha-cli\"")
            .write(to: reports.appendingPathComponent("iroha-cli-2026-09-08-223616.ips"), atomically: true, encoding: .utf8)

        let report = try XCTUnwrap(CrashReport.parse(fileURL: file))
        XCTAssertEqual(report.appName, "iroha")
        XCTAssertEqual(report.appVersion, "0.7.0-7")
        XCTAssertEqual(report.exceptionType, "EXC_CRASH")
        XCTAssertEqual(report.signal, "SIGABRT")
        XCTAssertEqual(report.terminationIndicator, "Abort trap: 6")
        XCTAssertEqual(report.ownFrames, ["ggml_abort", "ggml_metal_rsets_free"])
        XCTAssertEqual(
            report.summary,
            "file=iroha-2026-09-09-062759.ips version=0.7.0-7 exception=EXC_CRASH signal=SIGABRT"
                + " termination=Abort trap: 6 frames=ggml_abort < ggml_metal_rsets_free")

        let expected = try XCTUnwrap(CrashReport.parseTimestamp("2026-09-09 06:27:59.00 +0900"))
        XCTAssertEqual(report.date, expected)

        // 日時で絞る
        XCTAssertEqual(CrashReport.scan(directory: reports, appName: "iroha", newerThan: nil).count, 1)
        XCTAssertEqual(
            CrashReport.scan(directory: reports, appName: "iroha", newerThan: expected.addingTimeInterval(-1)).count, 1)
        XCTAssertEqual(
            CrashReport.scan(directory: reports, appName: "iroha", newerThan: expected).count, 0)
    }

    /// 前回起動以降に増えたレポートだけを起動時に記録する
    func testLaunchRecordsNewCrashReports() throws {
        let reports = dir.appendingPathComponent("reports")
        try FileManager.default.createDirectory(at: reports, withIntermediateDirectories: true)
        let reportDate = try XCTUnwrap(CrashReport.parseTimestamp("2026-09-09 06:27:59.00 +0900"))
        try Self.sampleIPS.write(
            to: reports.appendingPathComponent("iroha-2026-09-09-062759.ips"), atomically: true, encoding: .utf8)

        // 前回の起動がレポートより前 → 記録される
        log.recordLaunch(
            version: "1.0", osVersion: "os", pid: 100, now: reportDate.addingTimeInterval(-3600),
            crashReportsDirectory: nil)
        log.recordExit(reason: "terminate", pid: 100, now: reportDate.addingTimeInterval(-1))
        var lines = log.recordLaunch(
            version: "1.0", osVersion: "os", pid: 200, now: reportDate.addingTimeInterval(60),
            crashReportsDirectory: reports)
        XCTAssertEqual(lines.count, 2)
        XCTAssertTrue(lines[0].hasPrefix("crash-report file=iroha-2026-09-09-062759.ips"))

        // 次の起動では同じレポートを繰り返さない
        log.recordExit(reason: "terminate", pid: 200, now: reportDate.addingTimeInterval(120))
        lines = log.recordLaunch(
            version: "1.0", osVersion: "os", pid: 300, now: reportDate.addingTimeInterval(180),
            crashReportsDirectory: reports)
        XCTAssertEqual(lines.count, 1)
        XCTAssertTrue(lines[0].hasPrefix("launch"))
    }

    /// 実機のレポートを縮めたもの（ヘッダ1行 + 整形済みJSON本文）
    private static let sampleIPS = """
        {"app_name":"iroha","timestamp":"2026-09-09 06:27:59.00 +0900","app_version":"0.7.0-7","bug_type":"309","os_version":"macOS 26.6.2 (25G83)","name":"iroha"}
        {
          "procName" : "iroha",
          "exception" : {"type" : "EXC_CRASH", "signal" : "SIGABRT"},
          "termination" : {"indicator" : "Abort trap: 6", "byProc" : "iroha"},
          "faultingThread" : 0,
          "threads" : [
            {"triggered" : true, "frames" : [
              {"imageIndex" : 0, "symbol" : "__pthread_kill"},
              {"imageIndex" : 1, "symbol" : "abort"},
              {"imageIndex" : 2, "symbol" : "ggml_abort"},
              {"imageIndex" : 2, "symbol" : "ggml_metal_rsets_free"},
              {"imageIndex" : 1, "symbol" : "exit"}
            ]},
            {"frames" : []}
          ],
          "usedImages" : [
            {"name" : "libsystem_kernel.dylib"},
            {"name" : "libsystem_c.dylib"},
            {"name" : "iroha"}
          ]
        }
        """
}
