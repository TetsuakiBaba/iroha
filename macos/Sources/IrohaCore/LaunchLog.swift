import Foundation

/// 起動・終了の記録。データフォルダの `logs/` に置く軽量なログで、
/// irohaが予期せず終了・再起動していないかを後から確かめるためのもの。
///
/// NSLogはこの環境ではユニファイドログに残らないことがあるので、ファイルへ直接追記する。
///
/// - `launch-<ホスト名>.log`: 1行1イベントの追記ログ
///   - `launch`: 起動（PID・バージョン・macOS）
///   - `exit`: 終了とその理由（terminate / restart / uninstall / SIGTERM など）
///   - `unclean-exit`: 前回の起動の `exit` が記録されないまま次の起動に至った
///     （クラッシュ・強制終了・シャットダウンなど）
///   - `crash-report`: macOSが `~/Library/Logs/DiagnosticReports` に残したirohaのレポート
///     （前回の確認以降に増えたもの。終了処理中のabortのように `exit` の後で
///     落ちた場合は `unclean-exit` にならないので、これで補う）
///   - `note`: 別のirohaプロセスが動作中だった等の補足
/// - `state-<ホスト名>.json`: 直前の起動の記録（PID・開始時刻・終了時刻・レポート確認時刻）。
///   終了時に `exitedAt` を書くので、起動時にこれが無ければ前回は正常終了していない
///
/// データフォルダはiCloud/Dropboxで複数のMacと共有されることがあるため、ファイル名に
/// ホスト名を入れて端末ごとに分ける（同じファイルへ複数台が追記すると同期の競合になる）。
public struct LaunchLog: Sendable {

    /// 直前の起動の記録
    public struct State: Codable, Sendable, Equatable {
        public var pid: Int32
        public var startedAt: Date
        public var version: String
        /// クラッシュレポートを最後に確認した時刻。これより新しいレポートを次回起動時に記録する
        public var reportsCheckedAt: Date
        /// 終了を記録した時刻（実行中・異常終了ならnil）
        public var exitedAt: Date?
        public var exitReason: String?

        public init(pid: Int32, startedAt: Date, version: String, reportsCheckedAt: Date,
                    exitedAt: Date? = nil, exitReason: String? = nil) {
            self.pid = pid
            self.startedAt = startedAt
            self.version = version
            self.reportsCheckedAt = reportsCheckedAt
            self.exitedAt = exitedAt
            self.exitReason = exitReason
        }

        /// 終了が記録されていない（実行中か、異常終了した）
        public var isRunning: Bool { exitedAt == nil }
    }

    /// ログを置くフォルダ（既定はデータフォルダの `logs/`）
    public let directory: URL
    /// ファイル名に入れる端末の識別子
    public let hostName: String
    /// この行数を超えたら古い半分を捨てる
    public var maxLines: Int
    /// 前回の目印が無いときに、どれだけ過去のクラッシュレポートまで拾うか
    public var initialReportLookback: TimeInterval = 7 * 24 * 60 * 60

    public init(directory: URL = DataDirectory.logsURL, hostName: String = LaunchLog.currentHostName(),
                maxLines: Int = 2000) {
        self.directory = directory
        self.hostName = hostName
        self.maxLines = maxLines
    }

    public var logFileURL: URL { directory.appendingPathComponent("launch-\(hostName).log") }
    public var stateFileURL: URL { directory.appendingPathComponent("state-\(hostName).json") }

    // MARK: - 記録

    /// 起動を記録する。前回の記録に終了が無ければ `unclean-exit`、新しいクラッシュレポートがあれば
    /// `crash-report` を先に書き、記録を自分のものに置き換える。
    /// - Parameters:
    ///   - crashReportsDirectory: レポートを探すフォルダ（nilで探さない）
    ///   - isProcessAlive: 前回記録のPIDがまだ動いているか（別インスタンスとの区別。テスト用に差し替え可）
    /// - Returns: 書いた行（タイムスタンプ抜き）
    @discardableResult
    public func recordLaunch(
        version: String, osVersion: String, pid: Int32 = getpid(), now: Date = Date(),
        crashReportsDirectory: URL? = CrashReport.defaultDirectory,
        isProcessAlive: (Int32) -> Bool = LaunchLog.isProcessAlive
    ) -> [String] {
        var lines: [String] = []
        let previous = readState()
        if let previous, previous.isRunning {
            if previous.pid != pid, isProcessAlive(previous.pid) {
                lines.append(
                    "note 別のirohaが動作中 pid=\(previous.pid) 開始=\(Self.format(previous.startedAt))"
                        + " version=\(previous.version)")
            } else {
                lines.append(
                    "unclean-exit 前回の終了が記録されていない（クラッシュ・強制終了・シャットダウンなど）"
                        + " pid=\(previous.pid) 開始=\(Self.format(previous.startedAt)) version=\(previous.version)")
            }
        }
        if let crashReportsDirectory {
            let since = previous?.reportsCheckedAt ?? now.addingTimeInterval(-initialReportLookback)
            for report in CrashReport.scan(directory: crashReportsDirectory, appName: "iroha", newerThan: since) {
                lines.append("crash-report \(report.summary)")
            }
        }
        lines.append("launch pid=\(pid) version=\(version) macOS=\(osVersion) host=\(hostName)")
        writeState(State(pid: pid, startedAt: now, version: version, reportsCheckedAt: now))
        append(lines, at: now)
        return lines
    }

    /// 終了を記録する（直前の記録が自分のものであるときだけ終了時刻を書く）
    public func recordExit(reason: String, pid: Int32 = getpid(), now: Date = Date()) {
        append(["exit reason=\(reason) pid=\(pid)"], at: now)
        if var state = readState(), state.pid == pid {
            state.exitedAt = now
            state.exitReason = reason
            writeState(state)
        }
    }

    /// ログの全行（無ければ空）
    public func lines() -> [String] {
        guard let text = try? String(contentsOf: logFileURL, encoding: .utf8) else { return [] }
        return text.split(separator: "\n", omittingEmptySubsequences: true).map(String.init)
    }

    // MARK: - 直前の起動の記録

    public func readState() -> State? {
        guard let data = try? Data(contentsOf: stateFileURL) else { return nil }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return try? decoder.decode(State.self, from: data)
    }

    private func writeState(_ state: State) {
        ensureDirectory()
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        guard let data = try? encoder.encode(state) else { return }
        try? data.write(to: stateFileURL, options: .atomic)
    }

    // MARK: - 追記

    private func append(_ lines: [String], at date: Date) {
        ensureDirectory()
        let stamp = Self.format(date)
        let text = lines.map { "\(stamp)\t\($0)\n" }.joined()
        guard let data = text.data(using: .utf8) else { return }
        if let handle = try? FileHandle(forWritingTo: logFileURL) {
            defer { try? handle.close() }
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: data)
        } else {
            try? data.write(to: logFileURL, options: .atomic)
        }
        trimIfNeeded()
    }

    /// 行数が上限を超えたら新しい半分だけ残す（起動ごとに数行なので通常は何年も届かない）
    private func trimIfNeeded() {
        let all = lines()
        guard all.count > maxLines else { return }
        let kept = all.suffix(maxLines / 2).joined(separator: "\n") + "\n"
        try? kept.data(using: .utf8)?.write(to: logFileURL, options: .atomic)
    }

    private func ensureDirectory() {
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    }

    // MARK: - 補助

    /// ログのタイムスタンプ（ローカル時刻）
    public static func format(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return formatter.string(from: date)
    }

    /// PIDのプロセスが存在するか
    public static func isProcessAlive(_ pid: Int32) -> Bool {
        guard pid > 0 else { return false }
        return kill(pid, 0) == 0 || errno == EPERM
    }

    /// この端末のホスト名（`.local` を除く）。ファイル名に使うので `/` は避ける
    public static func currentHostName() -> String {
        var buffer = [CChar](repeating: 0, count: 256)
        guard gethostname(&buffer, buffer.count) == 0 else { return "unknown" }
        var name = String(cString: buffer)
        if name.hasSuffix(".local") { name.removeLast(".local".count) }
        name = name.replacingOccurrences(of: "/", with: "_")
        return name.isEmpty ? "unknown" : name
    }
}

/// macOSが `~/Library/Logs/DiagnosticReports` に残すクラッシュレポート（`.ips`）の要約。
///
/// `.ips` は1行目がJSONのヘッダ（app_name / timestamp / app_version）、
/// 2行目以降が整形済みJSONの本文（exception / termination / threads）。
public struct CrashReport: Sendable, Equatable {
    public let fileURL: URL
    public let appName: String
    public let appVersion: String
    public let date: Date
    /// EXC_CRASH / EXC_BAD_ACCESS など
    public let exceptionType: String?
    /// SIGABRT / SIGSEGV など
    public let signal: String?
    /// "Abort trap: 6" など
    public let terminationIndicator: String?
    /// 落ちたスレッドのうち、自分のバイナリ内のフレーム（上から最大3つ）
    public let ownFrames: [String]

    /// 標準のレポート置き場
    public static let defaultDirectory: URL? = URL(
        fileURLWithPath: NSHomeDirectory() + "/Library/Logs/DiagnosticReports", isDirectory: true)

    /// 1行に収めた要約（ログ用）
    public var summary: String {
        var parts = ["file=\(fileURL.lastPathComponent)", "version=\(appVersion)"]
        if let exceptionType { parts.append("exception=\(exceptionType)") }
        if let signal { parts.append("signal=\(signal)") }
        if let terminationIndicator { parts.append("termination=\(terminationIndicator)") }
        if !ownFrames.isEmpty { parts.append("frames=\(ownFrames.joined(separator: " < "))") }
        return parts.joined(separator: " ")
    }

    /// フォルダ内の `appName` のレポートを日時順に返す（`newerThan` より後のものだけ）
    public static func scan(directory: URL, appName: String, newerThan: Date?) -> [CrashReport] {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: directory.path) else { return [] }
        return names
            .filter { $0.hasPrefix(appName + "-") && $0.hasSuffix(".ips") }
            .compactMap { parse(fileURL: directory.appendingPathComponent($0)) }
            .filter { $0.appName == appName }
            .filter { report in newerThan.map { report.date > $0 } ?? true }
            .sorted { $0.date < $1.date }
    }

    /// `.ips` を読む。ヘッダが読めなければnil。本文が読めなくてもヘッダだけで返す
    public static func parse(fileURL: URL) -> CrashReport? {
        guard let text = try? String(contentsOf: fileURL, encoding: .utf8) else { return nil }
        let headerEnd = text.firstIndex(of: "\n") ?? text.endIndex
        guard let headerData = String(text[..<headerEnd]).data(using: .utf8),
              let header = (try? JSONSerialization.jsonObject(with: headerData)) as? [String: Any],
              let appName = header["app_name"] as? String
        else { return nil }
        let date = (header["timestamp"] as? String).flatMap(parseTimestamp)
            ?? DataDirectory.modificationDate(of: fileURL) ?? .distantPast
        let appVersion = header["app_version"] as? String ?? ""

        var exceptionType: String?
        var signal: String?
        var terminationIndicator: String?
        var ownFrames: [String] = []
        if headerEnd < text.endIndex,
           let bodyData = String(text[text.index(after: headerEnd)...]).data(using: .utf8),
           let body = (try? JSONSerialization.jsonObject(with: bodyData)) as? [String: Any] {
            let exception = body["exception"] as? [String: Any]
            exceptionType = exception?["type"] as? String
            signal = exception?["signal"] as? String
            terminationIndicator = (body["termination"] as? [String: Any])?["indicator"] as? String
            if let faulting = body["faultingThread"] as? Int,
               let threads = body["threads"] as? [[String: Any]], faulting < threads.count,
               let frames = threads[faulting]["frames"] as? [[String: Any]],
               let images = body["usedImages"] as? [[String: Any]] {
                for frame in frames {
                    guard let imageIndex = frame["imageIndex"] as? Int, imageIndex < images.count,
                          images[imageIndex]["name"] as? String == appName,
                          let symbol = frame["symbol"] as? String
                    else { continue }
                    ownFrames.append(symbol)
                    if ownFrames.count == 3 { break }
                }
            }
        }
        return CrashReport(
            fileURL: fileURL, appName: appName, appVersion: appVersion, date: date,
            exceptionType: exceptionType, signal: signal, terminationIndicator: terminationIndicator,
            ownFrames: ownFrames)
    }

    /// ヘッダの timestamp（例 "2026-09-09 06:27:59.00 +0900"）
    static func parseTimestamp(_ text: String) -> Date? {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd HH:mm:ss.SS Z"
        return formatter.date(from: text)
    }
}
