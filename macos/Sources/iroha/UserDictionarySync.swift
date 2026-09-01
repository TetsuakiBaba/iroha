import Foundation
import IrohaCore

/// ユーザ辞書まわりの設定キーと、起動時の自動取り込み。
enum UserDictionarySync {

    /// 起動時にmacOSのユーザ辞書を取り込むか（既定はOFF: 取り込みは明示的な操作にする）
    static let autoSyncKey = "syncSystemUserDictionary"

    static var isAutoSyncEnabled: Bool {
        UserDefaults.standard.bool(forKey: autoSyncKey)
    }

    /// 設定がONなら、起動時にmacOSのユーザ辞書を取り込む。
    /// IMEの起動を遅らせないようバックグラウンドで実行し、失敗しても入力は妨げない
    static func syncOnLaunchIfEnabled() {
        guard isAutoSyncEnabled else { return }
        Task.detached(priority: .utility) {
            do {
                let result = try SystemUserDictionary.sync()
                if result.added > 0 || result.removed > 0 {
                    NSLog("iroha: ユーザ辞書を同期 (追加\(result.added) 削除\(result.removed))")
                }
            } catch {
                NSLog("iroha: ユーザ辞書の同期に失敗: \(error.localizedDescription)")
            }
        }
    }
}
