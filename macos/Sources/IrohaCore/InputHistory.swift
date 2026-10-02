import Foundation

/// 入力履歴の1件: 読み → 確定した表記と、確定した回数・最後に確定した日時。
///
/// 予測変換（入力中に読みの先頭が一致する語を出す）はこれだけで作る。
/// 事前学習したモデルの予測（左文脈の続きを生成する）は、文として自然でも本人が打とうとしている語とは
/// 別の文になることが多く、変換記録 150 件で試すと 1 件しか当たらなかった（2026-10-02）。
/// 同じ記録で「読みの先頭 2 文字 → 過去に確定した語」を引くと上位 3 件に 41.6% が入る
public struct InputHistoryEntry: Codable, Equatable, Sendable {
    public var reading: String
    public var text: String
    public var count: Int
    public var lastUsed: Date

    public init(reading: String, text: String, count: Int = 1, lastUsed: Date = Date()) {
        self.reading = reading
        self.text = text
        self.count = count
        self.lastUsed = lastUsed
    }

    public struct Key: Hashable, Sendable {
        public var reading: String
        public var text: String

        public init(reading: String, text: String) {
            self.reading = reading
            self.text = text
        }
    }

    public var key: Key { Key(reading: reading, text: text) }
}

/// 確定した「読み → 表記」から、入力履歴に残す単位を切り出す。
///
/// 確定はライブ変換のまま Enter で区切った長さなので「モデルを」「可能なのかな？」のように
/// 助詞・句読点まで付いている。これをそのまま覚えると「モデルが」と打つときに当たらない。
/// そこで文節（`ReadingAligner`）ごとに分け、文節そのものと、末尾のひらがなを除いた語の部分
/// （「モデル」「可能」）の両方を残す（変換記録での試算: 文節のままだと上位 3 件に 22.7%、
/// 語の部分も残すと 41.6%）。
///
/// ローマ字で打った読みだけを対象にする。読みにひらがな以外（Shift+英字で入れた英字など）が混ざる文節と、
/// 表記に英数字が混ざる文節（F9・F10 で英数にしたもの）は残さない
public enum InputHistoryUnits {

    /// 文節の末尾から落とす句読点（読みにもそのまま現れる）
    static let trailingPunctuation: Set<Character> = ["、", "。", "，", "．", "！", "？", "・"]

    /// 残す単位の読みの最低文字数（予測は読みを 2 文字打ってから出し、打った読みより長いものだけ出すため、
    /// 1 文字の読みは候補になりえない）
    public static let minimumReadingLength = 2

    public static func units(reading: String, text: String) -> [InputHistoryEntry.Key] {
        guard !reading.isEmpty, !text.isEmpty else { return [] }
        var units: [InputHistoryEntry.Key] = []
        func add(_ reading: String, _ text: String) {
            guard reading.count >= minimumReadingLength, !text.isEmpty,
                  reading.allSatisfy(isReadingCharacter), !text.contains(where: isAlphanumeric)
            else { return }
            let key = InputHistoryEntry.Key(reading: reading, text: text)
            if !units.contains(key) { units.append(key) }
        }
        for segment in ReadingAligner.segmentReading(reading, conversion: text) {
            var segmentReading = Substring(segment.reading)
            var segmentText = Substring(segment.conversion)
            while let last = segmentText.last, trailingPunctuation.contains(last),
                  segmentReading.last == last {
                segmentText.removeLast()
                segmentReading.removeLast()
            }
            add(String(segmentReading), String(segmentText))
            // 語の部分: 表記の末尾のひらがな（送り仮名・助詞）を読みからも同じだけ除く
            guard segmentText.contains(where: { !isHiragana($0) }) else { continue }
            let suffix = String(segmentText.reversed().prefix { isHiragana($0) }.reversed())
            guard !suffix.isEmpty, segmentReading.hasSuffix(suffix) else { continue }
            add(String(segmentReading.dropLast(suffix.count)), String(segmentText.dropLast(suffix.count)))
        }
        return units
    }

    /// 読みに現れてよい文字（ひらがな・長音符）
    static func isReadingCharacter(_ character: Character) -> Bool {
        character == "ー" || isHiragana(character)
    }

    static func isHiragana(_ character: Character) -> Bool {
        guard character.unicodeScalars.count == 1, let scalar = character.unicodeScalars.first else { return false }
        return (0x3041...0x3096).contains(scalar.value)
    }

    /// 英数字（半角・全角）
    static func isAlphanumeric(_ character: Character) -> Bool {
        guard let scalar = character.unicodeScalars.first else { return false }
        switch scalar.value {
        case 0x30...0x39, 0x41...0x5A, 0x61...0x7A,  // 半角
             0xFF10...0xFF19, 0xFF21...0xFF3A, 0xFF41...0xFF5A:  // 全角
            return true
        default:
            return false
        }
    }
}

/// 入力履歴の永続化と検索（予測変換の候補を引く）。
///
/// - データフォルダの `input-history/<ホスト名>.json` に、そのMacで確定した分だけを書く。
///   データフォルダは iCloud Drive / Dropbox で共有されることがあり、確定のたびに書くファイルを
///   複数台で1つにすると同期の競合が起きる（`learning.json` で実際に起きている）。
///   引くときは全ホストのファイルを合算する（回数は足し、最後に使った日時は新しい方）
/// - 削除は他のMacのファイルを書き換えず、自分のファイルに「消した記録」（読み・表記・日時）を残す。
///   合算のとき、それより前に使われた分はどのMacのものも数えない（消したあとにまた確定すれば戻る）。
///   すべて消すときは日時だけを残し、それより前の分を数えない
/// - 書き込みは確定を待たせないよう、まとめて少し遅らせる
public final class InputHistoryStore: @unchecked Sendable {

    public static let didChangeNotification = Notification.Name("iroha.inputHistoryDidChange")

    /// 既定の保存先フォルダ（`DataDirectory` の設定に追随する）
    public static var defaultDirectory: URL { DataDirectory.inputHistoryURL }

    public static let shared = InputHistoryStore()

    /// 1台ぶんのファイルに残す上限（超えたら最後に使った日時が古いものから捨てる）
    public static let maxEntries = 10_000
    /// 「消した記録」の上限
    public static let maxRemovals = 2_000
    /// 予測に出す候補の数
    public static let candidateLimit = 3

    public let directory: URL
    public let hostName: String

    private let lock = NSLock()
    /// このMacで確定した分
    private var own: HostFile
    /// 他のMacのファイル（ホスト名 → 内容）
    private var others: [String: HostFile] = [:]
    /// 合算した結果（検索・一覧に使う）
    private var merged: [InputHistoryEntry.Key: InputHistoryEntry] = [:]
    /// 最後に読み込み/保存したときの各ファイルの更新日時（外部からの変更の検出用）
    private var loadedModificationDates: [String: Date] = [:]
    private let saveQueue = DispatchQueue(label: "iroha.inputhistory.save", qos: .utility)
    private var pendingSave: DispatchWorkItem?
    /// 書き込みを遅らせる時間（続けて確定したぶんを1回にまとめる）
    private let saveDelay: TimeInterval

    public init(directory: URL = InputHistoryStore.defaultDirectory,
                hostName: String = LaunchLog.currentHostName(), saveDelay: TimeInterval = 3) {
        self.directory = directory
        self.hostName = hostName
        self.saveDelay = saveDelay
        self.own = HostFile()
        reloadAll()
    }

    // MARK: - 記録

    /// 確定した「読み → 表記」を記録する（`InputHistoryUnits` で切り出した単位ごとに回数を1つ増やす）
    public func record(reading: String, text: String, at date: Date = Date()) {
        let units = InputHistoryUnits.units(reading: reading, text: text)
        guard !units.isEmpty else { return }
        lock.lock()
        for key in units {
            Self.add(key, count: 1, at: date, to: &own.entries)
        }
        Self.trim(&own)
        rebuildMerged()
        lock.unlock()
        scheduleSave()
        postDidChange()
    }

    /// 変換記録（`ConversionLog`）のうち、このMacの分を取り込む。
    /// 前回取り込んだ日時より後の記録だけを数える（2回押しても二重に数えない）。
    /// 他のMacの変換記録はそのMacで取り込む（ここで数えると合算で二重になる）
    /// - Returns: 取り込んだ確定の件数
    @discardableResult
    public func importConversionLog(_ log: ConversionLog = .shared) -> Int {
        let records = log.records().filter { $0.file.lastPathComponent.contains("-\(log.hostName)-") }
        lock.lock()
        let since = own.importedThrough ?? .distantPast
        var imported = 0
        var latest = since
        // 記録は数千件あるので、1件ごとに一覧を先頭から探さないよう索引を使う
        var entries = Dictionary(own.entries.map { ($0.key, $0) }, uniquingKeysWith: { first, _ in first })
        for record in records where record.entry.timestamp > since {
            let entry = record.entry
            let pieces = entry.segments.map { $0.map { ($0.reading, $0.result) } }
                ?? [(entry.reading, entry.committed)]
            for (reading, text) in pieces {
                for key in InputHistoryUnits.units(reading: reading, text: text) {
                    if var existing = entries[key] {
                        existing.count += 1
                        existing.lastUsed = max(existing.lastUsed, entry.timestamp)
                        entries[key] = existing
                    } else {
                        entries[key] = InputHistoryEntry(
                            reading: key.reading, text: key.text, count: 1, lastUsed: entry.timestamp)
                    }
                }
            }
            imported += 1
            latest = max(latest, entry.timestamp)
        }
        own.entries = Array(entries.values)
        own.importedThrough = latest
        Self.trim(&own)
        rebuildMerged()
        lock.unlock()
        scheduleSave(immediately: true)
        postDidChange()
        return imported
    }

    // MARK: - 検索

    /// 読みが `prefix` で始まり、それより長い語を、確定した回数の多い順（同じなら最近使った順）に返す。
    /// 表記が同じものは1つにまとめる
    public func candidates(prefix: String, limit: Int = InputHistoryStore.candidateLimit) -> [InputHistoryEntry] {
        guard !prefix.isEmpty, limit > 0 else { return [] }
        lock.lock()
        let matches = merged.values.filter { $0.reading.count > prefix.count && $0.reading.hasPrefix(prefix) }
        lock.unlock()
        var seen: Set<String> = []
        var results: [InputHistoryEntry] = []
        for entry in matches.sorted(by: Self.isRankedBefore) where !seen.contains(entry.text) {
            seen.insert(entry.text)
            results.append(entry)
            if results.count >= limit { break }
        }
        return results
    }

    /// 全ホストを合算した一覧（設定画面用。並びは決めない）
    public var entries: [InputHistoryEntry] {
        lock.lock()
        defer { lock.unlock() }
        return Array(merged.values)
    }

    public var count: Int {
        lock.lock()
        defer { lock.unlock() }
        return merged.count
    }

    // MARK: - 削除

    /// 指定した語を消す。他のMacで確定した分も、合算のときに数えなくなる
    public func remove(_ keys: [InputHistoryEntry.Key], at date: Date = Date()) {
        guard !keys.isEmpty else { return }
        let removing = Set(keys)
        lock.lock()
        own.entries.removeAll { removing.contains($0.key) }
        own.removals.removeAll { removing.contains($0.key) }
        own.removals += keys.map { Removal(reading: $0.reading, text: $0.text, removedAt: date) }
        if own.removals.count > Self.maxRemovals {
            own.removals = Array(own.removals.sorted { $0.removedAt > $1.removedAt }.prefix(Self.maxRemovals))
        }
        rebuildMerged()
        lock.unlock()
        scheduleSave(immediately: true)
        postDidChange()
    }

    /// すべて消す。他のMacで確定した分も、この日時より前のものは数えなくなる
    public func reset(at date: Date = Date()) {
        lock.lock()
        own.entries = []
        own.removals = []
        own.clearedAt = date
        rebuildMerged()
        lock.unlock()
        scheduleSave(immediately: true)
        postDidChange()
    }

    // MARK: - 同期

    /// 他のMacのファイルが変わっていれば読み直す（`DataDirectoryWatcher` が定期的に呼ぶ）
    /// - Returns: 読み直したらtrue
    @discardableResult
    public func reloadIfChanged() -> Bool {
        let dates = currentModificationDates()
        lock.lock()
        let changed = dates.filter { $0.key != hostName }
            != loadedModificationDates.filter { $0.key != hostName }
        lock.unlock()
        guard changed else { return false }
        lock.lock()
        others = [:]
        for (host, _) in dates where host != hostName {
            others[host] = Self.load(from: fileURL(forHost: host))
        }
        loadedModificationDates = dates
        rebuildMerged()
        lock.unlock()
        postDidChange()
        return true
    }

    /// 溜まっている書き込みをすぐに済ませて終わるまで待つ（テスト・終了処理用）
    public func flush() {
        lock.lock()
        let hasPending = pendingSave != nil
        pendingSave?.cancel()
        pendingSave = nil
        lock.unlock()
        if hasPending { saveNow() }
        saveQueue.sync {}
    }

    public func fileURL(forHost host: String) -> URL {
        directory.appendingPathComponent("\(host).json")
    }

    // MARK: - 内部

    struct Removal: Codable, Equatable {
        var reading: String
        var text: String
        var removedAt: Date
        var key: InputHistoryEntry.Key { InputHistoryEntry.Key(reading: reading, text: text) }
    }

    struct HostFile: Codable, Equatable {
        var version = 1
        var entries: [InputHistoryEntry] = []
        var removals: [Removal] = []
        /// すべて消した日時（これより前に使われた分は、どのMacのものも数えない）
        var clearedAt: Date?
        /// 変換記録をここまで取り込んだ
        var importedThrough: Date?
    }

    private static func isRankedBefore(_ lhs: InputHistoryEntry, _ rhs: InputHistoryEntry) -> Bool {
        if lhs.count != rhs.count { return lhs.count > rhs.count }
        if lhs.lastUsed != rhs.lastUsed { return lhs.lastUsed > rhs.lastUsed }
        return lhs.reading < rhs.reading
    }

    private static func add(_ key: InputHistoryEntry.Key, count: Int, at date: Date,
                            to entries: inout [InputHistoryEntry]) {
        if let index = entries.firstIndex(where: { $0.key == key }) {
            entries[index].count += count
            entries[index].lastUsed = max(entries[index].lastUsed, date)
        } else {
            entries.append(InputHistoryEntry(reading: key.reading, text: key.text, count: count, lastUsed: date))
        }
    }

    private static func trim(_ file: inout HostFile) {
        guard file.entries.count > maxEntries else { return }
        file.entries = Array(file.entries.sorted { $0.lastUsed > $1.lastUsed }.prefix(maxEntries))
    }

    /// 全ホストを合算し直す（lock を取った状態で呼ぶ）
    private func rebuildMerged() {
        let files = [own] + Array(others.values)
        let clearedAt = files.compactMap(\.clearedAt).max() ?? .distantPast
        var removedAt: [InputHistoryEntry.Key: Date] = [:]
        for removal in files.flatMap(\.removals) {
            removedAt[removal.key] = max(removedAt[removal.key] ?? .distantPast, removal.removedAt)
        }
        var result: [InputHistoryEntry.Key: InputHistoryEntry] = [:]
        for entry in files.flatMap(\.entries) {
            guard entry.lastUsed > clearedAt, entry.lastUsed > removedAt[entry.key] ?? .distantPast else { continue }
            if var existing = result[entry.key] {
                existing.count += entry.count
                existing.lastUsed = max(existing.lastUsed, entry.lastUsed)
                result[entry.key] = existing
            } else {
                result[entry.key] = entry
            }
        }
        merged = result
    }

    private func reloadAll() {
        let dates = currentModificationDates()
        own = Self.load(from: fileURL(forHost: hostName))
        others = [:]
        for (host, _) in dates where host != hostName {
            others[host] = Self.load(from: fileURL(forHost: host))
        }
        loadedModificationDates = dates
        rebuildMerged()
    }

    /// フォルダ内の各ホストのファイルの更新日時
    private func currentModificationDates() -> [String: Date] {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: directory.path) else { return [:] }
        var dates: [String: Date] = [:]
        for name in names where name.hasSuffix(".json") {
            let host = String(name.dropLast(".json".count))
            dates[host] = DataDirectory.modificationDate(of: directory.appendingPathComponent(name))
        }
        return dates
    }

    private func scheduleSave(immediately: Bool = false) {
        lock.lock()
        pendingSave?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.saveNow() }
        pendingSave = work
        lock.unlock()
        saveQueue.asyncAfter(deadline: .now() + (immediately ? 0 : saveDelay), execute: work)
    }

    private func saveNow() {
        lock.lock()
        let contents = own
        pendingSave = nil
        lock.unlock()
        let url = fileURL(forHost: hostName)
        Self.save(contents, to: url)
        lock.lock()
        loadedModificationDates[hostName] = DataDirectory.modificationDate(of: url)
        lock.unlock()
    }

    private func postDidChange() {
        DispatchQueue.main.async {
            NotificationCenter.default.post(name: Self.didChangeNotification, object: nil)
        }
    }

    private static func load(from url: URL) -> HostFile {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        guard let data = try? Data(contentsOf: url),
              let file = try? decoder.decode(HostFile.self, from: data) else { return HostFile() }
        return file
    }

    private static func save(_ file: HostFile, to url: URL) {
        do {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
            encoder.dateEncodingStrategy = .iso8601
            try encoder.encode(file).write(to: url, options: .atomic)
        } catch {
            NSLog("iroha: 入力履歴の保存に失敗: \(error)")
        }
    }
}
