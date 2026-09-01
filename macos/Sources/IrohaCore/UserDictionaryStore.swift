import Foundation

/// ユーザ辞書の永続化（JSONファイル）。
///
/// irohaのユーザ辞書はこのファイルが唯一の情報源で、macOSのユーザ辞書からは
/// `syncFromSystem(_:)`で取り込むだけ（macOS側には一切書き込まない）。
/// IMEと設定ウィンドウは同一プロセスなので、`shared`をプロセス内で共有すれば足りる。
public final class UserDictionaryStore: @unchecked Sendable {

    /// 内容が変わったときに通知する（設定ウィンドウの一覧更新用）
    public static let didChangeNotification = Notification.Name("iroha.userDictionaryDidChange")

    public static let defaultURL = URL(
        fileURLWithPath: NSHomeDirectory()
            + "/Library/Application Support/iroha/user-dictionary.json")

    public static let shared = UserDictionaryStore()

    /// 取り込み結果のサマリ（設定UIでの報告用）
    public struct SyncResult: Sendable, Equatable {
        public var added: Int
        public var removed: Int
        public var unchanged: Int
        public var skipped: Int   // 読みがひらがなでない等で対象外にしたもの
    }

    private let url: URL
    private let lock = NSLock()
    private var cached: UserDictionary

    public init(url: URL = UserDictionaryStore.defaultURL) {
        self.url = url
        self.cached = Self.load(from: url)
    }

    /// 変換時に参照するスナップショット
    public var current: UserDictionary {
        lock.lock()
        defer { lock.unlock() }
        return cached
    }

    public var entries: [UserDictionaryEntry] { current.entries }

    /// 一覧をまるごと置き換えて保存する（設定UIの編集結果の反映）
    @discardableResult
    public func replaceAll(_ entries: [UserDictionaryEntry]) -> UserDictionary {
        store(entries)
    }

    @discardableResult
    public func add(reading: String, word: String) -> UserDictionary {
        var entries = self.entries
        entries.append(UserDictionaryEntry(reading: reading, word: word, source: .manual))
        return store(entries)
    }

    @discardableResult
    public func remove(ids: Set<UUID>) -> UserDictionary {
        store(entries.filter { !ids.contains($0.id) })
    }

    /// macOSのユーザ辞書の内容に合わせる。
    ///
    /// - `.system`（取り込み由来）のエントリだけを追加・削除の対象にし、
    ///   irohaで追加・編集した`.manual`のエントリには触らない
    /// - macOS側から消えたエントリはこちらからも消す（ミラーリング）
    @discardableResult
    public func syncFromSystem(_ systemEntries: [(reading: String, word: String)]) -> SyncResult {
        var valid: [(reading: String, word: String)] = []
        var skipped = 0
        for entry in systemEntries {
            let reading = UserDictionary.normalizedReading(entry.reading)
            let word = entry.word.trimmingCharacters(in: .whitespacesAndNewlines)
            guard UserDictionary.isImportableReading(reading), !word.isEmpty else {
                skipped += 1
                continue
            }
            valid.append((reading, word))
        }

        lock.lock()
        let existing = cached.entries
        lock.unlock()

        // 手動エントリと同じ内容が既にあるものは追加しない（重複表示を避ける）
        let manualPairs = Set(existing.filter { $0.source == .manual }.map { Pair($0) })
        var wanted: [Pair: Void] = [:]
        var ordered: [Pair] = []
        for entry in valid {
            let pair = Pair(reading: entry.reading, word: entry.word)
            guard !manualPairs.contains(pair), wanted[pair] == nil else { continue }
            wanted[pair] = ()
            ordered.append(pair)
        }

        var result = SyncResult(added: 0, removed: 0, unchanged: 0, skipped: skipped)
        var updated: [UserDictionaryEntry] = []
        var kept: Set<Pair> = []
        for entry in existing {
            guard entry.source == .system else {
                updated.append(entry)
                continue
            }
            let pair = Pair(entry)
            if wanted[pair] != nil, !kept.contains(pair) {
                updated.append(entry)
                kept.insert(pair)
                result.unchanged += 1
            } else {
                result.removed += 1
            }
        }
        for pair in ordered where !kept.contains(pair) {
            updated.append(
                UserDictionaryEntry(reading: pair.reading, word: pair.word, source: .system))
            result.added += 1
        }

        if result.added > 0 || result.removed > 0 {
            store(updated)
        }
        return result
    }

    // MARK: - 内部

    /// (読み, 単語)の同一性判定用
    private struct Pair: Hashable {
        var reading: String
        var word: String
        init(reading: String, word: String) {
            self.reading = reading
            self.word = word
        }
        init(_ entry: UserDictionaryEntry) {
            self.init(reading: entry.reading, word: entry.word)
        }
    }

    @discardableResult
    private func store(_ entries: [UserDictionaryEntry]) -> UserDictionary {
        // 読みが空・単語が空のものは落とし、読み順に並べる（同じ読みの候補は登録順を保つ）
        let cleaned = entries
            .map {
                UserDictionaryEntry(
                    id: $0.id,
                    reading: UserDictionary.normalizedReading($0.reading),
                    word: $0.word.trimmingCharacters(in: .whitespacesAndNewlines),
                    source: $0.source)
            }
            .filter { !$0.reading.isEmpty && !$0.word.isEmpty }
        let sorted = cleaned.enumerated()
            .sorted { ($0.element.reading, $0.offset) < ($1.element.reading, $1.offset) }
            .map(\.element)

        let dictionary = UserDictionary(entries: sorted)
        lock.lock()
        cached = dictionary
        lock.unlock()
        save(sorted)
        // 受け手はUI（設定ウィンドウ）なのでメインスレッドに揃える
        // （起動時の自動同期はバックグラウンドから呼ばれる）
        DispatchQueue.main.async {
            NotificationCenter.default.post(name: Self.didChangeNotification, object: nil)
        }
        return dictionary
    }

    private struct FileContents: Codable {
        var version: Int
        var entries: [UserDictionaryEntry]
    }

    private static func load(from url: URL) -> UserDictionary {
        guard let data = try? Data(contentsOf: url),
              let contents = try? JSONDecoder().decode(FileContents.self, from: data)
        else { return .empty }
        return UserDictionary(entries: contents.entries)
    }

    private func save(_ entries: [UserDictionaryEntry]) {
        do {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
            let data = try encoder.encode(FileContents(version: 1, entries: entries))
            try data.write(to: url, options: .atomic)
        } catch {
            NSLog("iroha: ユーザ辞書の保存に失敗: \(error)")
        }
    }
}
