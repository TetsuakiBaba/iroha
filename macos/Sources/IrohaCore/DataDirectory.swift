import Foundation

/// irohaのデータ（変換モデル・ユーザ辞書・学習・変換ルール・設定の同期ファイル）を置くフォルダ。
///
/// 既定は `~/Library/Application Support/iroha`。ユーザが設定でiCloud DriveやDropboxの
/// フォルダを指定すると、複数のMacで同じデータを共有できる。
/// フォルダ内のレイアウトはどこに置いても同じ（`models/` と直下のJSON）。
///
/// 各ストアの `shared` は初回アクセス時にこのフォルダから読むので、
/// アプリ側は起動直後（ストアに触る前）に `configure(_:)` を呼ぶこと。
public enum DataDirectory {

    /// 既定の保存場所
    public static let defaultURL = URL(
        fileURLWithPath: NSHomeDirectory() + "/Library/Application Support/iroha",
        isDirectory: true)

    private static let lock = NSLock()
    nonisolated(unsafe) private static var configured: URL = defaultURL

    /// 現在の保存場所
    public static var url: URL {
        lock.lock()
        defer { lock.unlock() }
        return configured
    }

    /// 既定の場所を使っているか
    public static var isDefault: Bool {
        url.standardizedFileURL.path == defaultURL.standardizedFileURL.path
    }

    /// 保存場所を切り替える（フォルダが無ければ作る）。
    /// 作れない場合（外部ボリューム未接続など）は既定の場所に戻し、falseを返す
    @discardableResult
    public static func configure(_ url: URL) -> Bool {
        let target = url.standardizedFileURL
        var isDirectory: ObjCBool = false
        let exists = FileManager.default.fileExists(atPath: target.path, isDirectory: &isDirectory)
        var usable = exists && isDirectory.boolValue
        if !exists {
            usable = (try? FileManager.default.createDirectory(
                at: target, withIntermediateDirectories: true)) != nil
        }
        lock.lock()
        configured = usable ? target : defaultURL
        lock.unlock()
        return usable
    }

    // MARK: - フォルダ内のレイアウト

    /// 変換モデル（GGUF）を置くフォルダ
    public static var modelsURL: URL { url.appendingPathComponent("models", isDirectory: true) }
    /// 既定の変換モデル
    public static var defaultModelURL: URL {
        modelsURL.appendingPathComponent(defaultModelFileName)
    }
    public static let defaultModelFileName = "zenz-v3.1-small-Q5_K_M.gguf"

    public static var userDictionaryURL: URL { url.appendingPathComponent("user-dictionary.json") }
    public static var learningURL: URL { url.appendingPathComponent("learning.json") }
    public static var userRewriteRulesURL: URL { url.appendingPathComponent("user-rewrite-rules.json") }
    /// UserDefaultsの設定を他のMacと共有するためのスナップショット
    public static var preferencesURL: URL { url.appendingPathComponent("settings.json") }

    /// 保存場所を移すときにコピーする対象（フォルダ直下の相対パス）
    public static let portableItems = [
        "models", "user-dictionary.json", "learning.json", "user-rewrite-rules.json", "settings.json",
    ]

    /// `from` の内容を `to` へコピーする。移行先に同名の項目が既にあればそれを優先して触らない
    /// （他のMacが先に置いたデータを上書きしないため）。`models/` は中のファイル単位で同じ扱い。
    /// - Returns: コピーした項目の数
    @discardableResult
    public static func copyPortableItems(from: URL, to: URL) throws -> Int {
        let fm = FileManager.default
        try fm.createDirectory(at: to, withIntermediateDirectories: true)
        var copied = 0
        for item in portableItems {
            let source = from.appendingPathComponent(item)
            let destination = to.appendingPathComponent(item)
            guard fm.fileExists(atPath: source.path) else { continue }
            var isDirectory: ObjCBool = false
            fm.fileExists(atPath: source.path, isDirectory: &isDirectory)
            if isDirectory.boolValue {
                try fm.createDirectory(at: destination, withIntermediateDirectories: true)
                for name in try fm.contentsOfDirectory(atPath: source.path) where !name.hasSuffix(".tmp") {
                    let file = destination.appendingPathComponent(name)
                    guard !fm.fileExists(atPath: file.path) else { continue }
                    try fm.copyItem(at: source.appendingPathComponent(name), to: file)
                    copied += 1
                }
            } else {
                guard !fm.fileExists(atPath: destination.path) else { continue }
                try fm.copyItem(at: source, to: destination)
                copied += 1
            }
        }
        return copied
    }

    /// ファイルの更新日時（無ければnil）。ストアが外部からの変更を検出するのに使う
    public static func modificationDate(of url: URL) -> Date? {
        (try? FileManager.default.attributesOfItem(atPath: url.path))?[.modificationDate] as? Date
    }
}
