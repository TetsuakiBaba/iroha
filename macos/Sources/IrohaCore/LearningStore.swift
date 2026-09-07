import Foundation

/// 変換の学習結果の永続化（JSONファイル）。
///
/// 記録するのは「ユーザが文節変換で修正して確定した」ときだけで、
/// エンジンの出力をそのまま確定した場合は何も覚えない。
public final class LearningStore: @unchecked Sendable {

    public static let didChangeNotification = Notification.Name("iroha.learningDidChange")

    /// 既定の保存先（`DataDirectory` の設定に追随する）
    public static var defaultURL: URL { DataDirectory.learningURL }

    public static let shared = LearningStore()

    /// 保持する上限（超えたら古いものから捨てる）
    public static let maxSentences = 500
    public static let maxSegments = 2000

    private let url: URL
    private let lock = NSLock()
    private var cached: LearningDictionary
    /// 確定操作を待たせないよう、ファイル書き込みは直列キューで非同期に行う
    private let saveQueue = DispatchQueue(label: "iroha.learning.save", qos: .utility)
    /// 最後に読み込み/保存したときのファイルの更新日時（外部からの変更の検出用）
    private var loadedModificationDate: Date?

    public init(url: URL = LearningStore.defaultURL) {
        self.url = url
        self.cached = Self.load(from: url)
        self.loadedModificationDate = DataDirectory.modificationDate(of: url)
    }

    /// ファイルが外部（他のMacからの同期など）で変わっていれば取り込む。
    ///
    /// 学習は両方のMacで増えていくので、置き換えではなくマージする（同じキーは新しい方を採る）。
    /// マージ結果がファイルと違えば書き戻すので、往復するうちに両者は同じ内容に収束する。
    /// - Returns: 取り込んだらtrue
    @discardableResult
    public func reloadIfChanged() -> Bool {
        let date = DataDirectory.modificationDate(of: url)
        lock.lock()
        guard date != loadedModificationDate else {
            lock.unlock()
            return false
        }
        loadedModificationDate = date
        // ファイルが消えたのは他のMacでのリセット。マージで復活させず、こちらも空にする
        guard date != nil else {
            cached = .empty
            lock.unlock()
            postDidChange()
            return true
        }
        let external = Self.load(from: url).entries
        let mine = cached.entries
        lock.unlock()

        let merged = Self.merged(base: external, recorded: mine)
        lock.lock()
        let changedLocally = merged != cached.entries
        if changedLocally { cached = LearningDictionary(entries: merged) }
        lock.unlock()
        if merged != external {
            saveQueue.async { [url] in
                Self.save(merged, to: url)
                self.lock.lock()
                self.loadedModificationDate = DataDirectory.modificationDate(of: url)
                self.lock.unlock()
            }
        }
        if changedLocally { postDidChange() }
        return true
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
        saveQueue.async { [url] in
            try? FileManager.default.removeItem(at: url)
            self.lock.lock()
            self.loadedModificationDate = nil
            self.lock.unlock()
        }
        postDidChange()
    }

    // MARK: - 内部

    private func merge(_ recorded: [LearningEntry]) {
        lock.lock()
        let entries = Self.merged(base: cached.entries, recorded: recorded)
        cached = LearningDictionary(entries: entries)
        lock.unlock()

        saveQueue.async { [url] in
            Self.save(entries, to: url)
            self.lock.lock()
            self.loadedModificationDate = DataDirectory.modificationDate(of: url)
            self.lock.unlock()
        }
        postDidChange()
    }

    /// `base` に `recorded` を重ねる。同じ (種類, 読み, 文脈) は新しい方（updatedAt）を採り、
    /// 上限を超えたら古いものから捨てる
    static func merged(base: [LearningEntry], recorded: [LearningEntry]) -> [LearningEntry] {
        var byKey: [Key: LearningEntry] = [:]
        var order: [Key] = []
        for entry in base + recorded {
            let key = Key(entry)
            if let existing = byKey[key] {
                // ユーザの今回の修正（recorded側）は同時刻でも優先する
                guard entry.updatedAt >= existing.updatedAt else { continue }
            } else {
                order.append(key)
            }
            byKey[key] = entry
        }

        var sentences = order.compactMap { byKey[$0] }.filter { $0.kind == .sentence }
        var segments = order.compactMap { byKey[$0] }.filter { $0.kind == .segment }
        if sentences.count > Self.maxSentences {
            sentences = Array(sentences.sorted { $0.updatedAt > $1.updatedAt }.prefix(Self.maxSentences))
        }
        if segments.count > Self.maxSegments {
            segments = Array(segments.sorted { $0.updatedAt > $1.updatedAt }.prefix(Self.maxSegments))
        }
        return sentences + segments
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
