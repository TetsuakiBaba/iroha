import Foundation

/// 「こえ」モードの記録 1 件（`VoiceLog`）。
///
/// 2 種類ある:
/// - `utterance`: キーを離して認識が終わったとき。音声認識が返した文字列と、それを読みに戻したもの
/// - `commit`: 声で入れた入力を確定したとき。確定した文字列と、そこに入った `utterance` の `id`
///
/// 2 つを `id` でつなぐと「話した内容 → 直して確定した文」の組になる（フィラーの除去を評価する材料。
/// `experiments/filler-removal/`）。確定は打った文字や候補での直しも含めた、未確定文字列全体の確定
public struct VoiceLogEntry: Codable, Sendable, Equatable {

    public enum Kind: String, Codable, Sendable {
        case utterance
        case commit
    }

    /// 記録の形式。項目を変えたら上げる
    public var format: Int
    public var kind: Kind
    public var timestamp: Date

    // MARK: utterance

    /// この発話の識別子（`commit` の `utterances` から引く）
    public var id: String?
    /// 音声認識の種類（`VoiceRecognizerKind` の rawValue）
    public var recognizer: String?
    /// 音声認識が返した文字列（前後の空白を除いたもの）
    public var recognized: String?
    /// 読みと固定部分に分けた結果をつなげた表記（`VoiceTranscript.surface`。語の間の空白を除いたもの）
    public var surface: String?
    /// 読みの部分のかなと固定部分をつなげたもの（未確定文字列に入れた読み）
    public var reading: String?

    // MARK: commit

    /// この確定に入った発話の `id`（入れた順）
    public var utterances: [String]?
    /// 確定した文字列
    public var committed: String?

    public static func utterance(
        id: String, recognizer: String, recognized: String, transcript: VoiceTranscript, timestamp: Date = Date()
    ) -> VoiceLogEntry {
        let reading = transcript.parts.map { part -> String in
            switch part {
            case .reading(let kana, _): return kana
            case .fixed(let text): return text
            }
        }.joined()
        return VoiceLogEntry(
            format: VoiceLog.currentFormat, kind: .utterance, timestamp: timestamp, id: id, recognizer: recognizer,
            recognized: recognized, surface: transcript.surface, reading: reading)
    }

    public static func commit(utterances: [String], committed: String, timestamp: Date = Date()) -> VoiceLogEntry {
        VoiceLogEntry(
            format: VoiceLog.currentFormat, kind: .commit, timestamp: timestamp,
            utterances: utterances, committed: committed)
    }
}

/// 「こえ」モードの認識結果と確定を 1 行 1 件で残す追記専用の記録（開発者モードの設定・既定 OFF。2026-11-19）。
///
/// - データフォルダの `logs/voice/` に月ごとのファイル `voice-<ホスト名>-YYYY-MM.jsonl`
///   （`ConversionLog` と同じく、データフォルダを共有する別の Mac と同じファイルに書かないようにホスト名で分ける）
/// - 実行時には読まない。書き込みは直列キューで非同期に行い、入力を待たせない
/// - 音声そのものは残さない
public final class VoiceLog: @unchecked Sendable {

    public static let currentFormat = 1

    /// 既定の保存先フォルダ（データフォルダの `logs/voice/`）
    public static var defaultDirectory: URL {
        DataDirectory.logsURL.appendingPathComponent("voice", isDirectory: true)
    }

    public static let shared = VoiceLog()

    public let directory: URL
    public let hostName: String
    private let queue = DispatchQueue(label: "iroha.voicelog.write", qos: .utility)

    public init(directory: URL = VoiceLog.defaultDirectory, hostName: String = LaunchLog.currentHostName()) {
        self.directory = directory
        self.hostName = hostName
    }

    /// 1 件追記する（非同期。呼び出し側は待たない）
    public func record(_ entry: VoiceLogEntry) {
        queue.async { [self] in self.append(entry) }
    }

    /// 溜まっている書き込みが終わるまで待つ（テスト用）
    public func waitUntilIdle() {
        queue.sync {}
    }

    /// `date` の月のファイル
    public func fileURL(for date: Date) -> URL {
        directory.appendingPathComponent("voice-\(hostName)-\(Self.monthString(date)).jsonl")
    }

    /// このフォルダにある記録のファイル（全ホスト分。名前順）
    public func fileURLs() -> [URL] {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: directory.path) else { return [] }
        return names
            .filter { $0.hasPrefix("voice-") && $0.hasSuffix(".jsonl") }
            .sorted()
            .map { directory.appendingPathComponent($0) }
    }

    /// 記録されている発話の数（設定画面の表示用。ファイルを全部読むので頻繁には呼ばない）
    public func utteranceCount() -> Int {
        fileURLs().reduce(0) { $0 + entries(in: $1).filter { $0.kind == .utterance }.count }
    }

    /// ファイルを読む。壊れた行は飛ばす
    public func entries(in url: URL) -> [VoiceLogEntry] {
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return [] }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return text.split(separator: "\n", omittingEmptySubsequences: true).compactMap {
            guard let data = $0.data(using: .utf8) else { return nil }
            return try? decoder.decode(VoiceLogEntry.self, from: data)
        }
    }

    private func append(_ entry: VoiceLogEntry) {
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        guard var data = try? encoder.encode(entry) else { return }
        data.append(0x0A)  // "\n"
        let url = fileURL(for: entry.timestamp)
        if let handle = try? FileHandle(forWritingTo: url) {
            defer { try? handle.close() }
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: data)
        } else {
            try? data.write(to: url, options: .atomic)
        }
    }

    private static func monthString(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = .current
        formatter.dateFormat = "yyyy-MM"
        return formatter.string(from: date)
    }
}
