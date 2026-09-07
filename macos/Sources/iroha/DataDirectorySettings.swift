import AppKit
import Foundation
import IrohaCore

/// データの保存場所（`DataDirectory`）のユーザ設定。
///
/// パスはUserDefaultsの `dataDirectory` に持つ（この値だけは端末ごとの設定なので
/// `PreferencesSync` の同期対象に入れない）。切り替えはプロセス再起動で反映する。
/// ストアの `shared` は起動時に一度だけ作られるため、実行中に差し替えるより確実
enum DataDirectorySettings {

    static let key = "dataDirectory"

    /// 設定されている保存場所（未設定なら既定）
    static var configuredURL: URL {
        if let path = UserDefaults.standard.string(forKey: key), !path.isEmpty {
            return URL(fileURLWithPath: (path as NSString).expandingTildeInPath, isDirectory: true)
        }
        return DataDirectory.defaultURL
    }

    /// 起動直後（どのストアにも触る前）に呼ぶ。設定したフォルダが使えなければ既定に戻す
    static func applyAtLaunch() {
        let url = configuredURL
        guard !DataDirectory.configure(url) else { return }
        NSLog("iroha: データフォルダ \(url.path) が使えないため既定の場所を使います")
    }

    /// 表示用のパス（ホームは~に縮める）
    static var displayPath: String {
        (DataDirectory.url.path as NSString).abbreviatingWithTildeInPath
    }

    /// 保存場所を切り替える。`copyExisting` なら現在のデータのうち移行先に無いものをコピーする。
    /// 反映には再起動が必要（呼び出し側で `AppRestarter.restartInstalledApp()` を呼ぶ）
    static func change(to newURL: URL, copyExisting: Bool) throws {
        let target = newURL.standardizedFileURL
        try FileManager.default.createDirectory(at: target, withIntermediateDirectories: true)
        if copyExisting {
            // 設定の同期ファイルは今の値で書いてから運ぶ（移行先で最新の設定が復元されるように）
            PreferencesSync.shared.exportNow()
            try DataDirectory.copyPortableItems(from: DataDirectory.url, to: target)
        }
        if target.path == DataDirectory.defaultURL.standardizedFileURL.path {
            UserDefaults.standard.removeObject(forKey: key)
        } else {
            UserDefaults.standard.set(target.path, forKey: key)
        }
        UserDefaults.standard.synchronize()
    }

    /// 移行先に既にirohaのデータがあるか（他のMacが先に置いた場合。コピーの要否の判断用）
    static func hasExistingData(at url: URL) -> Bool {
        DataDirectory.portableItems.contains {
            FileManager.default.fileExists(atPath: url.appendingPathComponent($0).path)
        }
    }
}
