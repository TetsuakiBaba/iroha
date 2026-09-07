import Foundation
import IrohaCore

/// UserDefaultsの設定をデータフォルダの `settings.json` と同期する。
///
/// UserDefaults自体は端末ローカルのplistなので、iCloud DriveやDropboxで共有できるよう
/// 対象キーのスナップショットをファイルに書き出し、他のMacが書いた新しいファイルは取り込む。
///
/// - 書き出し: UserDefaultsが変わったら（1秒デバウンス）、対象キーの値が
///   ファイルと違うときだけ `updatedAt` を進めて書く
/// - 取り込み: ファイルの `updatedAt` が最後に反映した時刻（`stampKey`）より新しければ、
///   対象キーをファイルの内容にそろえる（ファイルに無いキーは消す）
/// - 端末固有の値（保存場所そのもの・モデルの絶対パス・アップデート確認の履歴・APIキー）は対象外
final class PreferencesSync {

    static let shared = PreferencesSync()

    /// 最後にファイルへ反映/ファイルから取り込んだ `updatedAt`（端末ローカル）
    private static let stampKey = "preferencesSyncStamp"

    /// 同期するキー
    static let syncedKeys: Set<String> = {
        var keys: Set<String> = [
            "liveConversion", "commitOnPunctuation", "candidateCount", "punctuationStyle",
            LearningSettings.enabledKey, UserDictionarySync.autoSyncKey,
            SelectionSettings.enabledKey, SelectionSettings.triggerModeKey,
            SelectionSettings.onDemandHotkeyKey, SelectionSettings.excludedBundleIdsKey,
            TranslationBackend.userDefaultsKey,
            "ollamaModel", "lmStudioModel", "openAIModel", "openAIEndpoint",
            "ollamaEndpoint", "lmStudioEndpoint",
            "autoUpdateCheck",
        ]
        for i in 0..<AICommitSettings.count {
            keys.formUnion([
                AICommitSettings.nameKey(i), AICommitSettings.promptKey(i),
                AICommitSettings.shortcutKey(i),
            ])
        }
        for i in 0..<SelectionSettings.count {
            keys.formUnion([
                SelectionSettings.nameKey(i), SelectionSettings.promptKey(i),
                SelectionSettings.hotkeyKey(i), SelectionSettings.enabledObjectKey(i),
            ])
        }
        return keys
    }()

    private var observer: NSObjectProtocol?
    private var exportTask: DispatchWorkItem?
    /// 取り込み中はUserDefaultsの変更通知で書き出しを起こさない
    private var isImporting = false
    private let queue = DispatchQueue.main

    private init() {}

    /// 起動時: ファイルが新しければ取り込み、以後はUserDefaultsの変更を書き出す
    func start() {
        importIfNewer()
        observer = NotificationCenter.default.addObserver(
            forName: UserDefaults.didChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in
            self?.scheduleExport()
        }
        // ファイルがまだ無ければ今の設定で作る（移行時にコピーされるように）
        if !FileManager.default.fileExists(atPath: DataDirectory.preferencesURL.path) {
            exportNow()
        }
    }

    // MARK: - 取り込み

    /// 他のMacが書いた新しいスナップショットがあれば反映する。
    /// - Returns: 反映したらtrue
    @discardableResult
    func importIfNewer() -> Bool {
        guard let snapshot = Snapshot.read(from: DataDirectory.preferencesURL) else { return false }
        let stamp = UserDefaults.standard.double(forKey: Self.stampKey)
        guard snapshot.updatedAt > stamp else { return false }

        let defaults = UserDefaults.standard
        isImporting = true
        defer { isImporting = false }
        var changed = 0
        for key in Self.syncedKeys {
            let incoming = snapshot.values[key]
            let current = defaults.object(forKey: key)
            guard !Snapshot.isEqual(incoming, current) else { continue }
            if let incoming {
                defaults.set(incoming, forKey: key)
            } else {
                defaults.removeObject(forKey: key)
            }
            changed += 1
        }
        defaults.set(snapshot.updatedAt, forKey: Self.stampKey)
        // 取り込み中に溜まった書き出し予約は捨てる（ファイルと同じ内容になったので不要）
        exportTask?.cancel()
        exportTask = nil
        NSLog("iroha: 設定を \(DataDirectory.preferencesURL.path) から取り込みました（\(changed)件）")
        return true
    }

    // MARK: - 書き出し

    private func scheduleExport() {
        guard !isImporting else { return }
        exportTask?.cancel()
        let task = DispatchWorkItem { [weak self] in self?.exportNow() }
        exportTask = task
        queue.asyncAfter(deadline: .now() + 1.0, execute: task)
    }

    /// 対象キーの現在値をファイルへ書く（ファイルと同じなら書かない）
    func exportNow() {
        exportTask?.cancel()
        exportTask = nil
        let url = DataDirectory.preferencesURL
        let values = Snapshot.currentValues()
        if let existing = Snapshot.read(from: url), Snapshot.isEqual(existing.values, values) {
            return
        }
        // 直前に他のMacの変更が届いていれば、上書きする前に取り込む（こちらの変更は残る）
        importIfNewer()
        let merged = Snapshot.currentValues()
        let snapshot = Snapshot(updatedAt: Date().timeIntervalSince1970, values: merged)
        do {
            try snapshot.write(to: url)
            UserDefaults.standard.set(snapshot.updatedAt, forKey: Self.stampKey)
        } catch {
            NSLog("iroha: 設定の書き出しに失敗: \(error)")
        }
    }

    // MARK: - ファイル形式

    struct Snapshot {
        var updatedAt: TimeInterval
        var values: [String: Any]

        static func currentValues() -> [String: Any] {
            var values: [String: Any] = [:]
            for key in PreferencesSync.syncedKeys {
                // Bool/Int/Double/StringだけをJSONに載せる（Dateなどはこの対象キーには無い）
                if let value = UserDefaults.standard.object(forKey: key),
                   value is NSNumber || value is String {
                    values[key] = value
                }
            }
            return values
        }

        static func read(from url: URL) -> Snapshot? {
            guard let data = try? Data(contentsOf: url),
                  let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let updatedAt = object["updatedAt"] as? Double,
                  let raw = object["values"] as? [String: Any]
            else { return nil }
            // 知らないキーや型は捨てる（古い/新しいバージョンのファイルへの耐性）
            let values = raw.filter { PreferencesSync.syncedKeys.contains($0.key) }
                .filter { $0.value is NSNumber || $0.value is String }
            return Snapshot(updatedAt: updatedAt, values: values)
        }

        func write(to url: URL) throws {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            let object: [String: Any] = ["version": 1, "updatedAt": updatedAt, "values": values]
            let data = try JSONSerialization.data(
                withJSONObject: object, options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes])
            try data.write(to: url, options: .atomic)
        }

        static func isEqual(_ a: Any?, _ b: Any?) -> Bool {
            switch (a, b) {
            case (nil, nil): return true
            case let (a?, b?): return (a as? NSObject)?.isEqual(b) ?? false
            default: return false
            }
        }

        static func isEqual(_ a: [String: Any], _ b: [String: Any]) -> Bool {
            NSDictionary(dictionary: a).isEqual(to: b)
        }
    }
}
