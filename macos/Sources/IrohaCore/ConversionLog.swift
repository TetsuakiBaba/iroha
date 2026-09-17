import Foundation

/// 確定した変換を1件ずつ記録する追記専用のログ（将来の追加学習・LoRA用のデータ）。
///
/// `LearningStore`（`learning.json`）は「次の変換で引く辞書」で、修正したときだけ記録し、同じ読み・文脈は
/// 上書きし、上限を超えたら捨て、Mac間でマージする。学習データに必要な性質はその逆で、
/// 修正しなかった確定も含めて起きたことをそのまま積み、上書きも捨てもせず、実行時には一切読まない。
/// 役割が違うので別のファイルにする。
///
/// - 1行1件のJSON（JSONL）。モデルに渡した左文脈（漢字かな交じり・末尾40文字）、読み、エンジンが提示していた
///   変換結果、確定された文字列を持つ。`training/prepare_data.py` と同じ
///   `U+EE02<左文脈> U+EE00<読み> U+EE01<結果>` の行に落とせる
/// - データフォルダの `logs/conversions/` に月ごとのファイル `conversions-<ホスト名>-YYYY-MM.jsonl`。
///   データフォルダはiCloud/Dropboxで共有されることがあるので、`LaunchLog` と同じくホスト名で端末ごとに分ける
///   （同じファイルへ複数台が追記すると同期の競合になる）
/// - 左文脈にはユーザが書いていた文書のカーソル手前の文章がそのまま入るので、記録は既定OFF（設定でON）。
///   設定側（`ConversionLogSettings`）がOFFのときは `record` を呼ばない
/// - ファイル書き込みは直列キューで非同期に行い、確定操作を待たせない
public final class ConversionLog: @unchecked Sendable {

    public static let didChangeNotification = Notification.Name("iroha.conversionLogDidChange")

    /// 記録の形式。項目を変えたら上げる（学習スクリプトが古い行を読み分けるため）
    public static let currentFormat = 1

    /// 既定の保存先フォルダ（データフォルダの `logs/conversions/`）
    public static var defaultDirectory: URL {
        DataDirectory.logsURL.appendingPathComponent("conversions", isDirectory: true)
    }

    public static let shared = ConversionLog()

    public let directory: URL
    public let hostName: String
    private let queue = DispatchQueue(label: "iroha.conversionlog.write", qos: .utility)

    public init(directory: URL = ConversionLog.defaultDirectory,
                hostName: String = LaunchLog.currentHostName()) {
        self.directory = directory
        self.hostName = hostName
    }

    // MARK: - 記録

    /// 1件追記する（非同期。呼び出し側は待たない）
    public func record(_ entry: ConversionLogEntry) {
        queue.async { [self] in
            self.append(entry)
            DispatchQueue.main.async {
                NotificationCenter.default.post(name: Self.didChangeNotification, object: nil)
            }
        }
    }

    /// 溜まっている書き込みが終わるまで待つ（テスト・終了処理用）
    public func waitUntilIdle() {
        queue.sync {}
    }

    /// `date` の月のファイル
    public func fileURL(for date: Date) -> URL {
        directory.appendingPathComponent("conversions-\(hostName)-\(Self.monthString(date)).jsonl")
    }

    private func append(_ entry: ConversionLogEntry) {
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        guard var data = try? Self.makeEncoder().encode(entry) else { return }
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

    // MARK: - 参照・削除（設定画面用）

    /// このフォルダにあるログファイル（全ホスト分。名前順＝時系列順）
    public func fileURLs() -> [URL] {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: directory.path) else { return [] }
        return names
            .filter { $0.hasPrefix("conversions-") && $0.hasSuffix(".jsonl") }
            .sorted()
            .map { directory.appendingPathComponent($0) }
    }

    /// ログファイルの合計バイト数
    public func totalSize() -> Int64 {
        fileURLs().reduce(0) { sum, url in
            let size = (try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? NSNumber
            return sum + (size?.int64Value ?? 0)
        }
    }

    /// 記録されている件数（行数）。設定画面の表示用で、ファイルを全部読むので頻繁には呼ばない
    public func entryCount() -> Int {
        fileURLs().reduce(0) { count, url in
            guard let data = try? Data(contentsOf: url) else { return count }
            return count + data.reduce(0) { $0 + ($1 == 0x0A ? 1 : 0) }
        }
    }

    /// ファイルを読む（学習スクリプトの検証・テスト用）。壊れた行は飛ばす
    public func entries(in url: URL) -> [ConversionLogEntry] {
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return [] }
        let decoder = Self.makeDecoder()
        return text.split(separator: "\n", omittingEmptySubsequences: true).compactMap {
            guard let data = $0.data(using: .utf8) else { return nil }
            return try? decoder.decode(ConversionLogEntry.self, from: data)
        }
    }

    /// すべてのログファイルを削除する（他のホストの分も含む。書き込み待ちがあれば先に終える）
    public func removeAll() {
        queue.async { [self] in
            for url in self.fileURLs() {
                try? FileManager.default.removeItem(at: url)
            }
            DispatchQueue.main.async {
                NotificationCenter.default.post(name: Self.didChangeNotification, object: nil)
            }
        }
    }

    // MARK: - 補助

    static func makeEncoder() -> JSONEncoder {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601   // makeDecoder と揃える
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        return encoder
    }

    static func makeDecoder() -> JSONDecoder {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return decoder
    }

    /// ファイル名の月（ローカル時刻）
    static func monthString(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM"
        return formatter.string(from: date)
    }
}

/// 確定1件の記録。
///
/// 学習に使うときの基本形は `context` + `reading` → `committed`。`proposed` と `edited` は
/// 「モデルがどう間違えたか」（選好学習のペアや誤り分析）に使う。
public struct ConversionLogEntry: Codable, Sendable, Equatable {

    /// どの操作で確定したか
    public enum Mode: String, Codable, Sendable {
        /// ライブ変換の表示をそのまま確定した
        case live
        /// 文節変換（スペースキー）で候補を選んで確定した
        case segments
        /// F6〜F10で指定した表示形（ひらがな・カタカナ・英数）で確定した
        case functionKey
    }

    /// 左文脈をどこから取ったか
    public enum ContextSource: String, Codable, Sendable {
        /// アプリのカーソル手前のテキスト（`DocumentContextSettings`）
        case document
        /// irohaが直前に確定した文字列の蓄積（アプリから読めないとき）
        case committed
        /// どちらも無い（起動直後・フォーカス移動直後など）
        case none
    }

    /// 文節変換の1文節
    public struct Segment: Codable, Sendable, Equatable {
        public var reading: String
        public var result: String
        public init(reading: String, result: String) {
            self.reading = reading
            self.result = result
        }
    }

    /// 記録の形式（`ConversionLog.currentFormat`）
    public var format: Int
    public var timestamp: Date
    public var mode: Mode
    /// エンジンに渡した左文脈（漢字かな交じり。改行なし。文書側の末尾40文字 ＋ 未確定文字列の固定部分）
    public var context: String
    public var contextSource: ContextSource
    /// ひらがなの読み（学習データはカタカナなので、使うときに変換する）
    public var reading: String
    /// エンジンが提示していた変換結果。提示が届く前に確定した等で不明なら nil
    public var proposed: String?
    /// 確定された文字列
    public var committed: String
    /// `committed != proposed`。`proposed` が不明なら nil
    public var edited: Bool?
    /// 文節変換で確定したときの文節（左から）
    public var segments: [Segment]?
    /// 変換モデルのファイル名（拡張子なし）
    public var model: String

    public init(
        timestamp: Date = Date(), mode: Mode, context: String, contextSource: ContextSource,
        reading: String, proposed: String?, committed: String, segments: [Segment]? = nil, model: String
    ) {
        self.format = ConversionLog.currentFormat
        self.timestamp = timestamp
        self.mode = mode
        self.context = context
        self.contextSource = contextSource
        self.reading = reading
        self.proposed = proposed
        self.committed = committed
        self.edited = proposed.map { $0 != committed }
        self.segments = segments
        self.model = model
    }

    /// `training/prepare_data.py` と同じ学習用の1行（`U+EE02<左文脈40文字> U+EE00<カタカナ読み> U+EE01<結果>`）。
    /// 文脈が空ならタグごと省く（文脈なしの学習例）
    public var trainingLine: String {
        var line = ""
        let trimmed = String(context.suffix(LeftContext.maxLength))
        if !trimmed.isEmpty { line += "\u{EE02}" + trimmed }
        return line + "\u{EE00}" + hiraganaToKatakana(reading) + "\u{EE01}" + committed
    }
}
