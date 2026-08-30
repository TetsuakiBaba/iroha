import Foundation
import IrohaCore
import SQLite3

/// macOS標準の「ユーザ辞書」（システム設定 > キーボード > ユーザ辞書）を読み取る。
///
/// 実体は `~/Library/KeyboardServices/TextReplacements.db`（Core DataのSQLite）。
/// 公開APIの`NSSpellChecker.userReplacementsDictionary`はASCIIショートカットしか返さず、
/// かな読みで登録した項目が取れないため、ファイルを直接読む。
///
/// 注意点:
/// - **読み取り専用**。macOS側の辞書は絶対に変更しない（iCloud同期対象のため）
/// - iCloud同期の関係で未チェックポイントのWALに実データが入っていることがあり、
///   db本体だけを開くと0件に見える。db/-wal/-shm を一時ディレクトリへコピーしてから開く
/// - 非公開スキーマなので、期待どおりでなければエラーにしてUI側で案内する
enum SystemUserDictionary {

    static let sourceURL = URL(
        fileURLWithPath: NSHomeDirectory() + "/Library/KeyboardServices/TextReplacements.db")

    enum ReadError: LocalizedError {
        case notFound
        case unreadable(String)
        case unexpectedSchema

        var errorDescription: String? {
            switch self {
            case .notFound:
                return "macOSのユーザ辞書が見つかりません。"
                    + "システム設定 > キーボード > ユーザ辞書 に単語が登録されているか確認してください。"
            case .unreadable(let reason):
                return "macOSのユーザ辞書を読み取れませんでした（\(reason)）。"
                    + "システム設定 > プライバシーとセキュリティ > フルディスクアクセス で"
                    + " iroha を許可すると読み取れる場合があります。"
            case .unexpectedSchema:
                return "macOSのユーザ辞書の形式が想定と異なります。"
                    + "OSのアップデートで変わった可能性があります。"
            }
        }
    }

    /// (読み, 単語) を読み出す。読みがひらがなでないもの（"omw"等のASCIIショートカット）も
    /// そのまま返し、取り込むかどうかの判断は`UserDictionaryStore.syncFromSystem`に任せる
    static func read() throws -> [(reading: String, word: String)] {
        guard FileManager.default.fileExists(atPath: sourceURL.path) else {
            throw ReadError.notFound
        }

        let workDirectory = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("iroha-userdict-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: workDirectory) }

        let copyURL: URL
        do {
            try FileManager.default.createDirectory(
                at: workDirectory, withIntermediateDirectories: true)
            copyURL = workDirectory.appendingPathComponent(sourceURL.lastPathComponent)
            try FileManager.default.copyItem(at: sourceURL, to: copyURL)
            // WAL/SHMは存在しないこともある（チェックポイント済み）
            for suffix in ["-wal", "-shm"] {
                let sidecar = URL(fileURLWithPath: sourceURL.path + suffix)
                guard FileManager.default.fileExists(atPath: sidecar.path) else { continue }
                try FileManager.default.copyItem(
                    at: sidecar,
                    to: URL(fileURLWithPath: copyURL.path + suffix))
            }
        } catch {
            // TCCで保護されている場合もここに来る
            throw ReadError.unreadable(error.localizedDescription)
        }

        var database: OpaquePointer?
        // コピーに対してはREADWRITEで開く（WALの復旧をSQLiteに任せる）
        guard sqlite3_open_v2(copyURL.path, &database, SQLITE_OPEN_READWRITE, nil) == SQLITE_OK,
              let database
        else {
            let message = database.map { String(cString: sqlite3_errmsg($0)) } ?? "open失敗"
            sqlite3_close(database)
            throw ReadError.unreadable(message)
        }
        defer { sqlite3_close(database) }

        let sql = """
            SELECT ZSHORTCUT, ZPHRASE FROM ZTEXTREPLACEMENTENTRY
            WHERE ZSHORTCUT IS NOT NULL AND ZPHRASE IS NOT NULL
              AND (ZWASDELETED IS NULL OR ZWASDELETED = 0)
            """
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(database, sql, -1, &statement, nil) == SQLITE_OK, let statement
        else {
            sqlite3_finalize(statement)
            throw ReadError.unexpectedSchema
        }
        defer { sqlite3_finalize(statement) }

        var entries: [(reading: String, word: String)] = []
        while sqlite3_step(statement) == SQLITE_ROW {
            guard let shortcut = sqlite3_column_text(statement, 0),
                  let phrase = sqlite3_column_text(statement, 1)
            else { continue }
            entries.append((String(cString: shortcut), String(cString: phrase)))
        }
        return entries
    }

    /// macOSのユーザ辞書をirohaのユーザ辞書へ取り込む（差分同期）
    @discardableResult
    static func sync(into store: UserDictionaryStore = .shared) throws -> UserDictionaryStore.SyncResult {
        store.syncFromSystem(try read())
    }
}
