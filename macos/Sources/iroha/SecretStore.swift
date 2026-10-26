import Foundation
import Security

/// APIキーなどの秘密情報をKeychainに保存する小さなヘルパー。
/// UserDefaults（平文plist）に置かないためだけのもので、汎用性は狙わない
///
/// **キーチェーンには main thread から触れない。**読み取りは、アプリの署名が変わったときなどに
/// 「キーチェーン内の…を使用しようとしています」のダイアログを出し、答えるまで戻らない。
/// 設定画面は IME と同じプロセスにあるので、main thread で読むとその間どのアプリの入力にも応えられず、
/// パスワードのダイアログ（SecurityAgent）も iroha を入力メソッドとして待つので、答えるまで数分かかった
/// （2026-10-27 実測: モデルタブを開いてから 4 分 19 秒 main thread が止まった）。
/// 読み書きは専用のキューで順に行い、読んだ値は覚えておく。main thread では覚えた値（`cachedValue`）だけを使う
enum SecretStore {
    private static let service = "dev.iroha.inputmethod.iroha"
    /// キーチェーンの読み書きを順に行うキュー（書き込みの途中に古い値を読まないように）
    private static let queue = DispatchQueue(label: "dev.iroha.secret-store", qos: .userInitiated)
    private static let lock = NSLock()
    /// アカウント → 値（保存されていなければ空文字列）。読んだものと書いたものだけを持つ
    private static var cache: [String: String] = [:]

    /// 覚えている値。まだキーチェーンから読んでいなければ nil（キーチェーンには触れないので main thread で使ってよい）
    static func cachedValue(_ account: String) -> String? {
        lock.lock()
        defer { lock.unlock() }
        return cache[account]
    }

    /// キーチェーンの値（読んだことがあれば覚えた値）。保存されていなければ nil。
    /// ダイアログに答えるまで戻らないことがあるので main thread では呼ばない（`load` を使う）
    static func get(_ account: String) -> String? {
        assert(!Thread.isMainThread, "SecretStore.get を main thread で呼ばない")
        let value = queue.sync { readIntoCache(account) }
        return value.isEmpty ? nil : value
    }

    /// 裏で読み込み、終わったら main thread で `completion` に値（保存されていなければ空文字列）を渡す
    static func load(_ accounts: [String], completion: (@MainActor ([String: String]) -> Void)? = nil) {
        queue.async {
            var values: [String: String] = [:]
            for account in accounts { values[account] = readIntoCache(account) }
            guard let completion else { return }
            DispatchQueue.main.async { MainActor.assumeIsolated { completion(values) } }
        }
    }

    /// 空文字列を渡すと削除になる。覚えた値はすぐ変え、キーチェーンへの書き込みは裏で行う
    static func set(_ value: String, for account: String) {
        lock.lock()
        let unchanged = cache[account] == value
        cache[account] = value
        lock.unlock()
        guard !unchanged else { return }
        queue.async { write(value, for: account) }
    }

    /// それまでに頼んだ書き込みが終わるまで待つ（削除してすぐ終了するアンインストール用）
    static func waitForWrites() {
        queue.sync {}
    }

    // MARK: - キーチェーン（`queue` の上でだけ呼ぶ）

    private static func readIntoCache(_ account: String) -> String {
        if let cached = cachedValue(account) { return cached }
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var result: AnyObject?
        var value = ""
        if SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
           let data = result as? Data {
            value = String(data: data, encoding: .utf8) ?? ""
        }
        lock.lock()
        // 読んでいる間に `set` された値があればそちらを残す
        if let current = cache[account] { value = current } else { cache[account] = value }
        lock.unlock()
        return value
    }

    private static func write(_ value: String, for account: String) {
        let base: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        SecItemDelete(base as CFDictionary)
        guard !value.isEmpty, let data = value.data(using: .utf8) else { return }
        var add = base
        add[kSecValueData as String] = data
        SecItemAdd(add as CFDictionary, nil)
    }
}
