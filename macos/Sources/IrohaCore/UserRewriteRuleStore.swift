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

    public static let shared = UserRewriteRuleStore()

    private let url: URL
    private let lock = NSLock()
    private var cached: UserRewriteRuleSet
    /// 最後に読み込み/保存したときのファイルの更新日時（外部からの変更の検出用）
    private var loadedModificationDate: Date?

    public init(url: URL = UserRewriteRuleStore.defaultURL) {
        self.url = url
        self.cached = Self.load(from: url)
        self.loadedModificationDate = DataDirectory.modificationDate(of: url)
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
        cached = Self.load(from: url)
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
        var rules: [UserRewriteRule]
    }

    private static func load(from url: URL) -> UserRewriteRuleSet {
        guard let data = try? Data(contentsOf: url),
              let contents = try? JSONDecoder().decode(FileContents.self, from: data)
        else { return .empty }
        return UserRewriteRuleSet(rules: contents.rules)
    }

    private func save(_ rules: [UserRewriteRule]) {
        do {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
            let data = try encoder.encode(FileContents(version: 1, rules: rules))
            try data.write(to: url, options: .atomic)
            lock.lock()
            loadedModificationDate = DataDirectory.modificationDate(of: url)
            lock.unlock()
        } catch {
            NSLog("iroha: 変換ルールの保存に失敗: \(error)")
        }
    }
}
