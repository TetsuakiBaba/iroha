import Foundation

/// 変換の学習結果の永続化（JSONファイル）。
///
/// 記録するのは「ユーザが文節変換で修正して確定した」ときだけで、
/// エンジンの出力をそのまま確定した場合は何も覚えない。
public final class LearningStore: @unchecked Sendable {

    public static let didChangeNotification = Notification.Name("iroha.learningDidChange")

    public static let defaultURL = URL(
        fileURLWithPath: NSHomeDirectory()
            + "/Library/Application Support/iroha/learning.json")

    public static let shared = LearningStore()

    /// 保持する上限（超えたら古いものから捨てる）
    public static let maxSentences = 500
    public static let maxSegments = 2000

    private let url: URL
    private let lock = NSLock()
    private var cached: LearningDictionary
    /// 確定操作を待たせないよう、ファイル書き込みは直列キューで非同期に行う
    private let saveQueue = DispatchQueue(label: "iroha.learning.save", qos: .utility)

    public init(url: URL = LearningStore.defaultURL) {
        self.url = url
        self.cached = Self.load(from: url)
    }

    public var current: LearningDictionary {
        lock.lock()
        defer { lock.unlock() }
        return cached
    }

    public var count: Int { current.entries.count }

    /// ユーザの修正を記録する。
    ///
    /// - Parameters:
    ///   - reading: 入力全体の読み（ひらがな）
    ///   - result: 確定された文字列
    ///   - segments: 確定時の文節（読みと変換結果）。左からの並び順であること
    public func record(
        reading: String, result: String, segments: [(reading: String, result: String)]
    ) {
        guard !reading.isEmpty, !result.isEmpty else { return }
        let now = Date()
        var recorded: [LearningEntry] = [
            LearningEntry(kind: .sentence, reading: reading, result: result, updatedAt: now)
        ]
        // 文節は「直前までに確定した文字列」を文脈として一緒に覚える。
        // これで同じ読みでも位置によって違う変換を再現できる（記者の貴社）
        var leftContext = ""
        for segment in segments {
            defer { leftContext = String((leftContext + segment.result).suffix(LearningDictionary.contextLength)) }
            guard !segment.reading.isEmpty, !segment.result.isEmpty else { continue }
            recorded.append(
                LearningEntry(
                    kind: .segment, reading: segment.reading, result: segment.result,
                    leftContext: leftContext, updatedAt: now))
        }
        merge(recorded)
    }

    public func reset() {
        lock.lock()
        cached = .empty
        lock.unlock()
        saveQueue.async { [url] in try? FileManager.default.removeItem(at: url) }
        postDidChange()
    }

    // MARK: - 内部

    private func merge(_ recorded: [LearningEntry]) {
        lock.lock()
        // 同じ (種類, 読み, 文脈) は新しいもので置き換える
        var byKey: [Key: LearningEntry] = [:]
        var order: [Key] = []
        for entry in cached.entries {
            let key = Key(entry)
            if byKey[key] == nil { order.append(key) }
            byKey[key] = entry
        }
        for entry in recorded {
            let key = Key(entry)
            if byKey[key] == nil { order.append(key) }
            byKey[key] = entry
        }

        var sentences = order.compactMap { byKey[$0] }.filter { $0.kind == .sentence }
        var segments = order.compactMap { byKey[$0] }.filter { $0.kind == .segment }
        // 上限を超えたら古いものから捨てる
        if sentences.count > Self.maxSentences {
            sentences = Array(sentences.sorted { $0.updatedAt > $1.updatedAt }.prefix(Self.maxSentences))
        }
        if segments.count > Self.maxSegments {
            segments = Array(segments.sorted { $0.updatedAt > $1.updatedAt }.prefix(Self.maxSegments))
        }
        let entries = sentences + segments
        cached = LearningDictionary(entries: entries)
        lock.unlock()

        saveQueue.async { [url] in Self.save(entries, to: url) }
        postDidChange()
    }

    /// エントリの同一性（同じ読み・同じ文脈なら上書き）
    private struct Key: Hashable {
        var kind: LearningEntry.Kind
        var reading: String
        var leftContext: String
        init(_ entry: LearningEntry) {
            kind = entry.kind
            reading = entry.reading
            leftContext = entry.kind == .sentence ? "" : entry.leftContext
        }
    }

    private func postDidChange() {
        DispatchQueue.main.async {
            NotificationCenter.default.post(name: Self.didChangeNotification, object: nil)
        }
    }

    private struct FileContents: Codable {
        var version: Int
        var entries: [LearningEntry]
    }

    private static func load(from url: URL) -> LearningDictionary {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601   // saveの encoder と揃える
        guard let data = try? Data(contentsOf: url),
              let contents = try? decoder.decode(FileContents.self, from: data)
        else { return .empty }
        return LearningDictionary(entries: contents.entries)
    }

    private static func save(_ entries: [LearningEntry], to url: URL) {
        do {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
            encoder.dateEncodingStrategy = .iso8601
            let data = try encoder.encode(FileContents(version: 1, entries: entries))
            try data.write(to: url, options: .atomic)
        } catch {
            NSLog("iroha: 学習結果の保存に失敗: \(error)")
        }
    }
}
