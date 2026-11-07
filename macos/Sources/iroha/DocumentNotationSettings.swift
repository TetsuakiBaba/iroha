import Foundation
import IrohaCore
import IrohaDocuments

/// 自分の原稿に合わせる（設定 > 辞書・学習、既定OFF・同期する。2026-11-07）。
///
/// `<データフォルダ>/documents/` に置いた文書（PDF・Word・テキスト）から、読みごとにどの表記を何回使ったか
/// （`NotationPreference`）を数え、かな漢字変換のモデルの採点に 2 つの項を加える（`NotationPreferenceEngine`）:
/// - 表記: 「及び」53 回・「および」30 回のように書いていれば「及び」が選ばれやすくなる（すべての文書を合わせて数える）
/// - 用語: 入力の話題がどれかの文書に近いとき（`DocumentTopicIndex`）、その文書で使った語が同じ読みの別の語より
///   選ばれやすくなる（「きふ」を「記譜」と書いた文書の話題なら「寄付」より「記譜」）
/// 必ず置き換える辞書ではなく、モデルの点数に足すだけなので、文脈から見て合わない書き換えはモデルが拒む。
/// 決め方と計測は `experiments/document-vocabulary/README.md`（リポジトリ外）と CLAUDE.md。
///
/// - 表記の重み（開発者モード、既定 2・同期する）: 論文 12 本の計測で、2 が論文の文で +6、文書と関係のない文で
///   ほぼ変わらない（4 では「おすすめ→お進め」のような別の語への書き換えが出始めた）
/// - 用語の重み（開発者モード、既定 2・同期する）: 同じ原稿の続きを書く場合の計測で +6（231 → 237）、
///   iroha-bench-v1 で −1。4 にすると +10 だが、研究報告の文で 期間→機関・作製→作成 のような書き換えが出た
/// - プライベートモードの間は止める（ユーザ辞書と同じく、変換を iroha 本来の結果にする）
/// - 文書のフォルダはデータフォルダの中なので、データフォルダを共有している Mac では同じ文書を使う
/// - `documents/` の直下にフォルダを作ると、フォルダごとに分けて 1 つを選んで使う（`DocumentProfiles`。
///   論文を書くときと日常とで切り替えるため。2026-11-07）。選んでいるフォルダは Mac ごとで同期しない
///   （同時に 2 台で別の原稿を書くことがある）。切り替えは設定画面・入力メニュー・ショートカット
///   （使わない → 1 つ目 → … → 使わない の順）。「使わない」を選べば変換は機能を入れる前と同じ結果になる
enum DocumentNotationSettings {

    static let enabledKey = "documentNotation"
    static let weightKey = "documentNotationWeight"
    static let defaultWeight = 2.0
    static let topicWeightKey = "documentNotationTopicWeight"
    static let defaultTopicWeight = 2.0

    static var isEnabled: Bool {
        UserDefaults.standard.bool(forKey: enabledKey)
    }

    static var weight: Double {
        let value = UserDefaults.standard.object(forKey: weightKey) as? Double ?? defaultWeight
        return value >= 0 ? value : defaultWeight
    }

    static var topicWeight: Double {
        let value = UserDefaults.standard.object(forKey: topicWeightKey) as? Double ?? defaultTopicWeight
        return value >= 0 ? value : defaultTopicWeight
    }

    static var folderURL: URL { DocumentCollection.defaultFolderURL }

    /// 選んでいる原稿のフォルダ（保存値。`DocumentProfileSelection`。Mac ごとで同期しない）
    static let profileKey = "documentNotationProfile"
    /// 原稿のフォルダを切り替えるショートカット（"Ctrl+Option+D" 形式。空なら割り当てなし・同期する）
    static let profileShortcutKey = "documentNotationProfileHotkey"
    static let defaultProfileShortcut = "Ctrl+Option+D"

    static var storedProfile: String? {
        get { UserDefaults.standard.string(forKey: profileKey) }
        set { UserDefaults.standard.set(newValue, forKey: profileKey) }
    }

    /// いま使う原稿のフォルダの名前（使わない・フォルダが無いときは nil）
    static var activeProfile: String? {
        DocumentProfileSelection.resolve(stored: storedProfile, names: DocumentNotationStore.shared.snapshot.names)
    }

    /// 次の原稿のフォルダに切り替え、切り替えた先の名前を返す（使わないなら nil）
    @discardableResult
    static func selectNextProfile() -> String? {
        let names = DocumentNotationStore.shared.snapshot.names
        storedProfile = DocumentProfileSelection.next(stored: storedProfile, names: names)
        return activeProfile
    }

    /// 画面に出す原稿のフォルダの名前（直下全体を使うときは「documents フォルダ」）
    static func displayName(_ profile: String) -> String {
        profile.isEmpty ? "documents フォルダ" : profile
    }

    static var profileShortcutText: String {
        UserDefaults.standard.string(forKey: profileShortcutKey) ?? defaultProfileShortcut
    }

    /// 切り替えに使うショートカット（決め方はプライベートモードと同じ。`PrivateModeSettings.isUsable`）
    static var profileShortcut: AICommitShortcut? {
        let shortcut = AICommitShortcut(rawValue: profileShortcutText)
        return PrivateModeSettings.isUsable(shortcut) ? shortcut : nil
    }

    /// 変換エンジンが変換のたびに読む設定（OFF・プライベートモード・使わない・未読み込みなら何もしない）
    static var configuration: NotationPreferenceConfiguration {
        guard isEnabled, !PrivateModeSettings.isEnabled else { return .disabled }
        let snapshot = DocumentNotationStore.shared.snapshot
        guard let name = DocumentProfileSelection.resolve(stored: storedProfile, names: snapshot.names),
              let profile = snapshot.profiles[name] else { return .disabled }
        return NotationPreferenceConfiguration(
            preference: profile.preference, weight: weight, documentPreferences: profile.documentPreferences,
            topicIndex: profile.topicIndex, topicWeight: topicWeight)
    }
}

/// 原稿のフォルダごとに、文書を読んで数えた表記の回数と話題の索引を持つ。
/// 読み込みは裏のスレッドで行い、すべてのフォルダをまとめて読んでおく（PDF 12 本で約 2 秒）。切り替えは読み直さない
final class DocumentNotationStore: ObservableObject, @unchecked Sendable {

    static let shared = DocumentNotationStore()

    /// 原稿のフォルダ 1 つの要約（設定画面に出す）
    struct ProfileSummary: Equatable {
        let name: String
        let documents: Int
        let variantReadings: Int
    }

    enum Status: Equatable {
        case idle
        case loading
        /// ignoredFiles: フォルダに分けたため使っていない、直下のファイル
        case ready(profiles: [ProfileSummary], ignoredFiles: [String], failures: [String])
    }

    typealias VariantGroup = (reading: String, surfaces: [(surface: String, count: Int)])

    /// 設定画面に出す状態（main thread で更新する）
    @Published private(set) var status: Status = .idle
    /// 原稿のフォルダごとの、表記の使い分けがある読みの一覧（設定画面の一覧用。main thread で更新する）
    @Published private(set) var variantGroups: [String: [VariantGroup]] = [:]

    /// 原稿のフォルダ 1 つぶんの、変換のたびに読むもの
    struct Profile {
        /// そのフォルダの文書を合わせた表記の回数
        var preference: NotationPreference
        /// 文書ごとの表記の回数（用語の選び分けに使う）
        var documentPreferences: [NotationPreference]
        var topicIndex: DocumentTopicIndex?
    }

    struct Snapshot {
        /// 原稿のフォルダの名前（並びは `DocumentProfiles.folders` の順）
        var names: [String] = []
        var profiles: [String: Profile] = [:]
    }

    private let lock = NSLock()
    private var current = Snapshot()
    /// 最後に読んだときのフォルダの中身（名前・大きさ・更新日時）。変わったときだけ読み直す
    private var loadedSignature: String?
    private var isLoading = false
    private let queue = DispatchQueue(label: "iroha.document-notation", qos: .utility)

    private init() {}

    var snapshot: Snapshot {
        lock.lock()
        defer { lock.unlock() }
        return current
    }

    /// 設定が ON で、フォルダの中身が前回と変わっていれば読み直す（起動時・設定を ON にしたとき・
    /// データフォルダの定期確認から呼ぶ）。`force` なら中身が同じでも読み直す
    func reloadIfNeeded(force: Bool = false) {
        guard DocumentNotationSettings.isEnabled else { return }
        queue.async { [self] in
            let root = DocumentNotationSettings.folderURL
            let signature = Self.signature(of: root)
            lock.lock()
            let skip = isLoading || (!force && signature == loadedSignature)
            if !skip { isLoading = true }
            lock.unlock()
            guard !skip else { return }
            DispatchQueue.main.async { self.status = .loading }

            let (folders, ignoredFiles) = DocumentProfiles.folders(in: root)
            var snapshot = Snapshot()
            var summaries: [ProfileSummary] = []
            var groups: [String: [VariantGroup]] = [:]
            var failures: [String] = []
            for folder in folders {
                let collection = DocumentCollection(folder: folder.url)
                let preference = collection.notation
                let variantGroups = preference.variantGroups()
                snapshot.names.append(folder.name)
                snapshot.profiles[folder.name] = Profile(
                    preference: preference, documentPreferences: collection.documents.map(\.notation),
                    topicIndex: DocumentTopicIndex(documents: collection.documents.map(\.sentences)))
                summaries.append(ProfileSummary(
                    name: folder.name, documents: collection.documents.count, variantReadings: variantGroups.count))
                groups[folder.name] = variantGroups
                failures += collection.failures.map { folder.name.isEmpty ? $0.name : "\(folder.name)/\($0.name)" }
            }
            lock.lock()
            current = snapshot
            loadedSignature = signature
            isLoading = false
            lock.unlock()
            NSLog("iroha: 原稿を読み込みました（\(summaries.map { "\($0.name.isEmpty ? "直下" : $0.name) \($0.documents) 件" }.joined(separator: "・"))）")
            DispatchQueue.main.async {
                self.variantGroups = groups
                self.status = .ready(profiles: summaries, ignoredFiles: ignoredFiles, failures: failures)
            }
        }
    }

    /// フォルダの中身の要約（対応する文書の相対パス・大きさ・更新日時）
    static func signature(of folder: URL) -> String {
        guard let enumerator = FileManager.default.enumerator(
            at: folder, includingPropertiesForKeys: [.fileSizeKey, .contentModificationDateKey],
            options: [.skipsHiddenFiles, .skipsPackageDescendants])
        else { return "" }
        var lines: [String] = []
        for case let url as URL in enumerator where DocumentTextReader.isSupported(url) {
            let values = try? url.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey])
            let time = values?.contentModificationDate?.timeIntervalSince1970 ?? 0
            lines.append("\(url.path)\t\(values?.fileSize ?? 0)\t\(time)")
        }
        return lines.sorted().joined(separator: "\n")
    }
}
