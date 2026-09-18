import Foundation

/// 変換の学習結果の永続化（JSONファイル）。
///
/// 記録するのは「ユーザが文節変換で修正して確定した」ときだけで、
/// エンジンの出力をそのまま確定した場合は何も覚えない。
/// 1件は「入力の読み全体 → 確定文字列」（`LearningEntry`）。
public final class LearningStore: @unchecked Sendable {

    public static let didChangeNotification = Notification.Name("iroha.learningDidChange")

    /// 既定の保存先（`DataDirectory` の設定に追随する）
    public static var defaultURL: URL { DataDirectory.learningURL }

    public static let shared = LearningStore()

    /// 保持する上限（超えたら古いものから捨てる）
    public static let maxEntries = 500

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
    /// 覚えるのは入力の読み全体 → 確定文字列だけで、次に同じ読みを丸ごと入力したときに再現する。
    /// - Parameters:
    ///   - reading: 入力全体の読み（ひらがな）
    ///   - result: 確定された文字列
    public func record(reading: String, result: String) {
        guard !reading.isEmpty, !result.isEmpty else { return }
        merge([LearningEntry(reading: reading, result: result)])
    }

    /// 一覧を丸ごと置き換える（設定画面の編集用）。読み・結果が空のエントリは落とす
    public func replaceAll(_ entries: [LearningEntry]) {
        let cleaned = entries.filter { !$0.reading.isEmpty && !$0.result.isEmpty }
        lock.lock()
        cached = LearningDictionary(entries: cleaned)
        lock.unlock()
        saveQueue.async { [url] in
            Self.save(cleaned, to: url)
            self.lock.lock()
            self.loadedModificationDate = DataDirectory.modificationDate(of: url)
            self.lock.unlock()
        }
        postDidChange()
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

    /// `base` に `recorded` を重ねる。同じ読みは新しい方（updatedAt）を採り、
    /// 上限を超えたら古いものから捨てる
    static func merged(base: [LearningEntry], recorded: [LearningEntry]) -> [LearningEntry] {
        var byReading: [String: LearningEntry] = [:]
        var order: [String] = []
        for entry in base + recorded {
            if let existing = byReading[entry.reading] {
                // ユーザの今回の修正（recorded側）は同時刻でも優先する
                guard entry.updatedAt >= existing.updatedAt else { continue }
            } else {
                order.append(entry.reading)
            }
            byReading[entry.reading] = entry
        }

        var entries = order.compactMap { byReading[$0] }
        if entries.count > Self.maxEntries {
            entries = Array(entries.sorted { $0.updatedAt > $1.updatedAt }.prefix(Self.maxEntries))
        }
        return entries
    }

    private func postDidChange() {
        DispatchQueue.main.async {
            NotificationCenter.default.post(name: Self.didChangeNotification, object: nil)
        }
    }

    private struct FileContents: Decodable {
        var version: Int
        var entries: [StoredEntry]
    }

    private struct SavedContents: Encodable {
        var version: Int
        var entries: [LearningEntry]
    }

    /// ファイル上の1エントリ。文節の学習があった頃の `kind` を読めるようにしてある
    private struct StoredEntry: Decodable {
        var kind: String?
        var reading: String
        var result: String
        var updatedAt: Date
    }

    private static func load(from url: URL) -> LearningDictionary {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601   // saveの encoder と揃える
        guard let data = try? Data(contentsOf: url),
              let contents = try? decoder.decode(FileContents.self, from: data)
        else { return .empty }
        // 旧形式の文節のエントリ（読みの一部）は捨てる。読み全体の学習として扱うと
        // 文中の一部を覚えた語が入力全体の変換結果になってしまう
        let entries = contents.entries.filter { $0.kind != "segment" }.map {
            LearningEntry(reading: $0.reading, result: $0.result, updatedAt: $0.updatedAt)
        }
        return LearningDictionary(entries: entries)
    }

    private static func save(_ entries: [LearningEntry], to url: URL) {
        do {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
            encoder.dateEncodingStrategy = .iso8601
            let data = try encoder.encode(SavedContents(version: 1, entries: entries))
            try data.write(to: url, options: .atomic)
        } catch {
            NSLog("iroha: 学習結果の保存に失敗: \(error)")
        }
    }
}
