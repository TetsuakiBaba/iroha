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

    /// 変換エンジンが変換のたびに読む設定（OFF・プライベートモード・未読み込みなら何もしない）
    static var configuration: NotationPreferenceConfiguration {
        guard isEnabled, !PrivateModeSettings.isEnabled else { return .disabled }
        let store = DocumentNotationStore.shared.snapshot
        return NotationPreferenceConfiguration(
            preference: store.preference, weight: weight, documentPreferences: store.documentPreferences,
            topicIndex: store.topicIndex, topicWeight: topicWeight)
    }
}

/// 文書を読んで数えた表記の回数と話題の索引を持つ。読み込みは裏のスレッドで行う（PDF 12 本で約 2 秒）
final class DocumentNotationStore: ObservableObject, @unchecked Sendable {

    static let shared = DocumentNotationStore()

    enum Status: Equatable {
        case idle
        case loading
        case ready(documents: Int, variantReadings: Int, failures: [String])
    }

    /// 設定画面に出す状態（main thread で更新する）
    @Published private(set) var status: Status = .idle
    /// 表記の使い分けがある読みの一覧（設定画面の一覧用。main thread で更新する）
    @Published private(set) var variantGroups: [(reading: String, surfaces: [(surface: String, count: Int)])] = []

    /// 変換のたびに読むもの（すべての文書を合わせた表記の回数・文書ごとの回数・話題の索引）
    struct Snapshot {
        var preference: NotationPreference = .empty
        var documentPreferences: [NotationPreference] = []
        var topicIndex: DocumentTopicIndex?
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
            let signature = Self.signature(of: DocumentNotationSettings.folderURL)
            lock.lock()
            let skip = isLoading || (!force && signature == loadedSignature)
            if !skip { isLoading = true }
            lock.unlock()
            guard !skip else { return }
            DispatchQueue.main.async { self.status = .loading }

            let collection = DocumentCollection(folder: DocumentNotationSettings.folderURL)
            let preference = collection.notation
            let groups = preference.variantGroups()
            let snapshot = Snapshot(
                preference: preference, documentPreferences: collection.documents.map(\.notation),
                topicIndex: DocumentTopicIndex(documents: collection.documents.map(\.sentences)))
            lock.lock()
            current = snapshot
            loadedSignature = signature
            isLoading = false
            lock.unlock()
            NSLog("iroha: 原稿を読み込みました（文書 \(collection.documents.count) 件・表記の使い分け \(groups.count) 語）")
            DispatchQueue.main.async {
                self.variantGroups = groups
                self.status = .ready(
                    documents: collection.documents.count, variantReadings: groups.count,
                    failures: collection.failures.map(\.name))
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
