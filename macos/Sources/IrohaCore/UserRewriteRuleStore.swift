import Foundation

/// ユーザ定義の変換ルールの永続化（JSONファイル）。
///
/// ユーザ辞書（`UserDictionaryStore`）と同じ作り: このファイルが唯一の情報源で、
/// IMEと設定ウィンドウは同一プロセスなので `shared` をプロセス内で共有すれば足りる。
/// ルールの並び順は候補の並び順になるので、登録順をそのまま保つ（並べ替えない）。
public final class UserRewriteRuleStore: @unchecked Sendable {

    /// 内容が変わったときに通知する（設定ウィンドウの一覧更新用）
    public static let didChangeNotification = Notification.Name("iroha.userRewriteRulesDidChange")

    /// 既定の保存先（`DataDirectory` の設定に追随する）
    public static var defaultURL: URL { DataDirectory.userRewriteRulesURL }

    public static let shared = UserRewriteRuleStore(seedsDefaults: true)

    // MARK: - 既定のルール

    /// 既定ルールの世代。既定に項目を足したらこの値を上げる。
    /// ファイルに記録した世代より新しければ、まだ無いトリガーだけを追加する
    /// （ユーザが消した既定ルールを復活させないため、追加は世代が上がったときの1回だけ）
    public static let defaultRulesVersion = 1

    /// 初回起動時（および世代が上がったとき）に用意する既定のルール
    public static let defaultRules: [(trigger: String, output: String)] = [
        ("きょう", "{{date}}"),
        ("きのう", "{{date-1}}"),
        ("おととい", "{{date-2}}"),
        ("あした", "{{date+1}}"),
        ("あす", "{{date+1}}"),
        ("あさって", "{{date+2}}"),
        ("しあさって", "{{date+3}}"),
        ("いま", "{{time}}"),
    ]

    private let url: URL
    private let lock = NSLock()
    private var cached: UserRewriteRuleSet
    /// ファイルに記録されている既定ルールの世代（保存時に書き戻す）
    private var defaultsVersion: Int
    /// 最後に読み込み/保存したときのファイルの更新日時（外部からの変更の検出用）
    private var loadedModificationDate: Date?

    /// - Parameter seedsDefaults: 既定ルールをまだ入れていなければ追加する（IME本体はtrue。
    ///   テストやCLIで空のストアが欲しいときはfalse）
    public init(url: URL = UserRewriteRuleStore.defaultURL, seedsDefaults: Bool = false) {
        self.url = url
        let loaded = Self.load(from: url)
        self.cached = loaded.rules
        self.defaultsVersion = loaded.defaultsVersion
        self.loadedModificationDate = DataDirectory.modificationDate(of: url)
        if seedsDefaults { seedDefaultsIfNeeded() }
    }

    private func seedDefaultsIfNeeded() {
        guard defaultsVersion < Self.defaultRulesVersion else { return }
        var rules = cached.rules
        let existing = Set(rules.map(\.trigger))
        for item in Self.defaultRules where !existing.contains(item.trigger) {
            rules.append(UserRewriteRule(trigger: item.trigger, output: item.output))
        }
        defaultsVersion = Self.defaultRulesVersion
        store(rules)
    }

    /// ファイルが外部（他のMacからの同期など）で変わっていれば読み直す。
    /// - Returns: 読み直したらtrue
    @discardableResult
    public func reloadIfChanged() -> Bool {
        let date = DataDirectory.modificationDate(of: url)
        lock.lock()
        guard date != loadedModificationDate else {
            lock.unlock()
            return false
        }
        let loaded = Self.load(from: url)
        cached = loaded.rules
        defaultsVersion = max(defaultsVersion, loaded.defaultsVersion)
        loadedModificationDate = date
        lock.unlock()
        DispatchQueue.main.async {
            NotificationCenter.default.post(name: Self.didChangeNotification, object: nil)
        }
        return true
    }

    /// 候補生成時に参照するスナップショット
    public var current: UserRewriteRuleSet {
        lock.lock()
        defer { lock.unlock() }
        return cached
    }

    public var rules: [UserRewriteRule] { current.rules }

    /// 一覧をまるごと置き換えて保存する（設定UIの編集結果の反映）
    @discardableResult
    public func replaceAll(_ rules: [UserRewriteRule]) -> UserRewriteRuleSet {
        store(rules)
    }

    @discardableResult
    public func add(trigger: String, output: String) -> UserRewriteRuleSet {
        var rules = self.rules
        rules.append(UserRewriteRule(trigger: trigger, output: output))
        return store(rules)
    }

    @discardableResult
    public func remove(ids: Set<UUID>) -> UserRewriteRuleSet {
        store(rules.filter { !ids.contains($0.id) })
    }

    @discardableResult
    public func setEnabled(_ isEnabled: Bool, id: UUID) -> UserRewriteRuleSet {
        var rules = self.rules
        guard let index = rules.firstIndex(where: { $0.id == id }) else { return current }
        rules[index].isEnabled = isEnabled
        return store(rules)
    }

    // MARK: - 内部

    @discardableResult
    private func store(_ rules: [UserRewriteRule]) -> UserRewriteRuleSet {
        // トリガーが空・出力が空のものは落とす（編集途中の空行は保存しない）
        let cleaned = rules
            .map {
                UserRewriteRule(
                    id: $0.id, kind: $0.kind,
                    trigger: UserRewriteRule.normalizedTrigger($0.trigger),
                    output: $0.output, isEnabled: $0.isEnabled)
            }
            .filter { !$0.trigger.isEmpty && !$0.output.isEmpty }

        let ruleSet = UserRewriteRuleSet(rules: cleaned)
        lock.lock()
        cached = ruleSet
        lock.unlock()
        save(cleaned)
        DispatchQueue.main.async {
            NotificationCenter.default.post(name: Self.didChangeNotification, object: nil)
        }
        return ruleSet
    }

    private struct FileContents: Codable {
        var version: Int
        /// 既定ルールをどの世代まで入れたか（無い古いファイルは0＝未投入）
        var defaultsVersion: Int?
        var rules: [UserRewriteRule]
    }

    private static func load(from url: URL) -> (rules: UserRewriteRuleSet, defaultsVersion: Int) {
        guard let data = try? Data(contentsOf: url),
              let contents = try? JSONDecoder().decode(FileContents.self, from: data)
        else { return (.empty, 0) }
        return (UserRewriteRuleSet(rules: contents.rules), contents.defaultsVersion ?? 0)
    }

    private func save(_ rules: [UserRewriteRule]) {
        do {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
            lock.lock()
            let contents = FileContents(version: 1, defaultsVersion: defaultsVersion, rules: rules)
            lock.unlock()
            let data = try encoder.encode(contents)
            try data.write(to: url, options: .atomic)
            lock.lock()
            loadedModificationDate = DataDirectory.modificationDate(of: url)
            lock.unlock()
        } catch {
            NSLog("iroha: 変換ルールの保存に失敗: \(error)")
        }
    }
}
