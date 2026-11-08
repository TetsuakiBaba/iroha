import Foundation

/// 入れている専門辞書を、辞書ラティスが読める形（`TermDictionary`）にして持つ。
///
/// 書き出しは辞書の組み合わせ（ID と SHA-256）ごとに 1 回だけ行い、キャッシュのフォルダに残す
/// （次に起動したときは読むだけ）。書き出しは数十万語で数秒かかりうるので、`reload()` は裏のスレッドから呼ぶ
/// （`reloadInBackground()`）。終わるまでは前の状態（起動直後は専門辞書なし）を返す
public final class TermDictionaryStore: @unchecked Sendable {

    public static let shared = TermDictionaryStore()

    /// 読み直したときに通知する（設定画面の表示更新用）。main thread で送る
    public static let didChangeNotification = Notification.Name("iroha.termDictionaryDidChange")

    /// 書き出しの置き場所の既定（`~/Library/Caches/iroha/term-dictionary-lattice/`）。
    /// 消えても入れた辞書から作り直せるのでキャッシュに置き、データフォルダ（共有されうる）には置かない
    public static var defaultCacheURL: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("iroha/term-dictionary-lattice", isDirectory: true)
    }

    private let cacheURL: URL
    private let dictionaryURL: () -> URL?
    private let lock = NSLock()
    /// reload を同時に走らせない（書き出しの途中のフォルダを別の reload が消さないため）
    private let reloadLock = NSLock()
    private var cached: TermDictionary = .empty
    private var records: [TermDictionaryInstall.Record] = []
    /// 最後に読み込んだときの辞書の組。変わっていなければ読み直さない
    private var loadedSignature: String?

    public init(
        cacheURL: URL = TermDictionaryStore.defaultCacheURL,
        dictionaryURL: @escaping () -> URL? = { LatticeConverter.defaultDictionaryURL() }
    ) {
        self.cacheURL = cacheURL
        self.dictionaryURL = dictionaryURL
    }

    /// 辞書ラティスに渡すもの
    public var current: TermDictionary {
        lock.lock()
        defer { lock.unlock() }
        return cached
    }

    /// 読み込んだ辞書の素性
    public var installedRecords: [TermDictionaryInstall.Record] {
        lock.lock()
        defer { lock.unlock() }
        return records
    }

    /// 置き場所にある辞書を読み直す。中身が変わっていなければ何もしない
    /// - Returns: 読み直したら true
    @discardableResult
    public func reload() -> Bool {
        reloadLock.lock()
        defer { reloadLock.unlock() }
        let found = TermDictionaryInstall.installedRecords()
        let signature = found.map { "\($0.id):\($0.sha256)" }.joined(separator: ",")
        lock.lock()
        let unchanged = signature == loadedSignature
        lock.unlock()
        guard !unchanged else { return false }

        let dictionary: TermDictionary
        if found.isEmpty {
            dictionary = .empty
            removeCaches(except: nil)
        } else if let built = prepare(records: found, signature: signature) {
            dictionary = built
        } else {
            dictionary = .empty
        }
        lock.lock()
        cached = dictionary
        records = found
        loadedSignature = signature
        lock.unlock()
        DispatchQueue.main.async {
            NotificationCenter.default.post(name: Self.didChangeNotification, object: nil)
        }
        return true
    }

    public func reloadInBackground() {
        DispatchQueue.global(qos: .utility).async { [self] in reload() }
    }

    // MARK: - 書き出し

    /// 書き出し済みならそれを使い、無ければ書き出す
    private func prepare(records: [TermDictionaryInstall.Record], signature: String) -> TermDictionary? {
        guard let dictionaryURL = dictionaryURL() else {
            NSLog("iroha: 辞書ラティスの辞書が無いので専門辞書を使えません")
            return nil
        }
        let name = SHA256.hex(Data(signature.utf8)).prefix(16)
        let directory = cacheURL.appendingPathComponent(String(name), isDirectory: true)
        let marker = directory.appendingPathComponent("entries.txt")
        if let text = try? String(contentsOf: marker, encoding: .utf8), let count = Int(text) {
            removeCaches(except: directory)
            return TermDictionary(latticeDirectory: directory, entryCount: count)
        }

        var packs: [(kind: TermDictionary.Kind, pairs: [(reading: String, word: String)])] = []
        for record in records {
            guard let text = try? String(contentsOf: TermDictionaryInstall.fileURL(id: record.id), encoding: .utf8)
            else { continue }
            packs.append((record.kind, TermDictionary.parse(text)))
        }
        // 途中で止まっても半端なフォルダを使わないよう、別の名前で書いてから移す
        let staging = cacheURL.appendingPathComponent("staging-\(UUID().uuidString)", isDirectory: true)
        do {
            let count = try TermDictionary.export(packs: packs, dictionaryURL: dictionaryURL, to: staging)
            try String(count).write(to: staging.appendingPathComponent("entries.txt"), atomically: true, encoding: .utf8)
            try? FileManager.default.removeItem(at: directory)
            try FileManager.default.moveItem(at: staging, to: directory)
            removeCaches(except: directory)
            return TermDictionary(latticeDirectory: directory, entryCount: count)
        } catch {
            NSLog("iroha: 専門辞書を書き出せませんでした: \(error)")
            try? FileManager.default.removeItem(at: staging)
            return nil
        }
    }

    /// 使わなくなった書き出しを消す
    private func removeCaches(except keep: URL?) {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: cacheURL.path) else { return }
        for name in names where name != keep?.lastPathComponent {
            try? FileManager.default.removeItem(at: cacheURL.appendingPathComponent(name))
        }
    }
}
