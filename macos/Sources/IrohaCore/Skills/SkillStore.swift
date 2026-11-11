import Foundation

/// スキルの解除の状態（`SkillState`）の保存と、機能の側からの問い合わせ。
///
/// 置き場所は `<データフォルダ>/skills.json`（同じデータフォルダを使う Mac で共有する）。
/// 2 台で別々にコードを入れても消えないよう、ファイルを読むときは置き換えずに、この Mac が覚えている状態と合わせる
/// （`SkillState.merged`）。この Mac が覚えている状態は UserDefaults にも写しておく（`mirrorKey`。同期しない）。
/// 再起動をはさんでも、別の Mac がファイルを上書きして消したコードを戻せるようにするため。
/// **試すために状態を消すときは、ファイルと UserDefaults の `skillStateMirror` の両方を消す。**
///
/// 写しを UserDefaults に書くと `UserDefaults.didChangeNotification` が出るので、設定の変更で動き出す部品
/// （タップ入力・選択テキスト）は、スキルを解除したときにも合わせ直される
public final class SkillStore: @unchecked Sendable {

    /// 状態が変わったときに通知する（設定画面の更新用。メインスレッドで出す）
    public static let didChangeNotification = Notification.Name("iroha.skillsDidChange")

    public static var defaultURL: URL { DataDirectory.skillsURL }
    public static let mirrorKey = "skillStateMirror"

    public static let shared = SkillStore()

    private let url: URL
    private let mirror: UserDefaults?
    private let lock = NSLock()
    private var cached: SkillState
    private var loadedModificationDate: Date?

    /// - Parameter mirror: この Mac が覚えている状態の写しを置く UserDefaults（テストでは nil）
    public init(url: URL = SkillStore.defaultURL, mirror: UserDefaults? = .standard) {
        self.url = url
        self.mirror = mirror
        let file = Self.load(from: url)
        let remembered = mirror?.data(forKey: Self.mirrorKey).flatMap(Self.decode) ?? .empty
        cached = (file ?? .empty).merged(with: remembered)
        loadedModificationDate = DataDirectory.modificationDate(of: url)
        // ファイルに無いコードをこの Mac が覚えていたら書き戻す。ID もコードも無いうちはファイルを作らない
        if cached != file, cached != .empty { save(cached) }
    }

    // MARK: - 読み取り

    public var state: SkillState {
        lock.lock()
        defer { lock.unlock() }
        return cached
    }

    /// そのスキルが解除されているか（キーを打つたびに呼ばれるので、覚えている状態だけを見る）
    public func isUnlocked(_ skill: String) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return cached.isUnlocked(skill)
    }

    public var isAllUnlocked: Bool {
        lock.lock()
        defer { lock.unlock() }
        return cached.isAllUnlocked
    }

    // MARK: - 変更

    /// ID を返す。まだ無ければ作って保存する。
    /// 作るのは設定画面で ID を見せるときまで遅らせる（起動のたびに作ると、同期が届く前の別の Mac が別の ID を作りやすい）
    @discardableResult
    public func ensureID() -> String {
        if let id = state.id { return id }
        return update { state in
            if state.id == nil {
                state.id = SkillCode.newID()
                state.idCreatedAt = Self.now()
            }
            return state.id ?? ""
        }
    }

    public func addCode(_ text: String) -> SkillState.AddResult {
        update { $0.add(code: text, date: Self.now()) }
    }

    @discardableResult
    public func choose(skill: String) -> Bool {
        update { $0.choose(skill: skill, date: Self.now()) }
    }

    /// ファイルが外で（別の Mac からの同期などで）変わっていれば、覚えている状態と合わせる。
    /// - Returns: 状態が変わったら true
    @discardableResult
    public func reloadIfChanged() -> Bool {
        let date = DataDirectory.modificationDate(of: url)
        lock.lock()
        guard date != loadedModificationDate else {
            lock.unlock()
            return false
        }
        let before = cached
        let file = Self.load(from: url)
        let merged = before.merged(with: file ?? .empty)
        cached = merged
        loadedModificationDate = date
        lock.unlock()
        // ファイルに無いコードを覚えていたら書き戻す
        if merged != file, merged != .empty { save(merged) }
        guard merged != before else { return false }
        remember(merged)
        notify()
        return true
    }

    // MARK: - 内部

    /// 日時は秒までにする（JSON に書いて読み戻したときに同じ値になるように）
    private static func now() -> Date {
        Date(timeIntervalSince1970: Date().timeIntervalSince1970.rounded(.down))
    }

    private func update<T>(_ change: (inout SkillState) -> T) -> T {
        lock.lock()
        // 保存の前に、ファイルの今の中身を合わせる（別の Mac が足したコードを消さない）
        var next = cached.merged(with: Self.load(from: url) ?? .empty)
        let result = change(&next)
        next = next.normalized()
        let changed = next != cached
        cached = next
        lock.unlock()
        if changed {
            save(next)
            remember(next)
            notify()
        }
        return result
    }

    private func notify() {
        DispatchQueue.main.async {
            NotificationCenter.default.post(name: Self.didChangeNotification, object: nil)
        }
    }

    private func remember(_ state: SkillState) {
        guard let mirror, let data = try? Self.encoder.encode(state) else { return }
        mirror.set(data, forKey: Self.mirrorKey)
    }

    private static let encoder: JSONEncoder = {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        return encoder
    }()

    private static func decode(_ data: Data) -> SkillState? {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return (try? decoder.decode(SkillState.self, from: data))?.normalized()
    }

    private static func load(from url: URL) -> SkillState? {
        (try? Data(contentsOf: url)).flatMap(decode)
    }

    private func save(_ state: SkillState) {
        do {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Self.encoder.encode(state).write(to: url, options: .atomic)
            lock.lock()
            loadedModificationDate = DataDirectory.modificationDate(of: url)
            lock.unlock()
        } catch {
            NSLog("iroha: スキルの状態を保存できません: \(error)")
        }
    }
}
