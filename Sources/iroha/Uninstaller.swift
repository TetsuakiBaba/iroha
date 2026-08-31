import AppKit
import Carbon
import Foundation

/// アプリ内アンインストール（設定 > 情報 のボタンから呼ぶ）。
/// 入力ソースの選択を英数キーボードへ移してからirohaを入力ソース一覧から無効化し、
/// ~/Library/Input Methods/iroha.app を削除してプロセスを終了する。
/// アクセシビリティ権限の項目だけはAPIで消せないため、案内にとどめる。
enum Uninstaller {

    /// 実行して戻らない（完了・失敗いずれも案内を出してプロセスを終了する）
    static func run(purgeData: Bool) -> Never {
        // 1. 選択中の入力ソースがirohaのままだとシステムが再起動を試みるので、
        //    先にABCキーボードへ切り替えて選択を外す
        selectABCKeyboard()

        // 2. 入力ソース一覧（システム設定で「+」した項目）からirohaを外す
        disableIrohaInputSources()

        // 3. データの削除（任意）: モデル・ユーザ辞書・学習・設定・APIキー
        if purgeData {
            try? FileManager.default.removeItem(
                atPath: NSHomeDirectory() + "/Library/Application Support/iroha")
            SecretStore.set("", for: RemoteTranslator.openAIKeyAccount)
            if let bundleId = Bundle.main.bundleIdentifier {
                UserDefaults.standard.removePersistentDomain(forName: bundleId)
            }
        }

        // 4. 本体の削除（実行中でもバンドルは削除できる。ロード済みのコードはそのまま動く）
        var removeError: Error?
        do {
            try FileManager.default.removeItem(at: SelfInstaller.installedURL)
        } catch {
            removeError = error
        }

        // 5. 案内を出して終了。exit()でなく_exit()なのはAppRestarterと同じ理由
        //    （ggmlのC++静的デストラクタがMetal解放でabortすることがある）
        let alert = NSAlert()
        if let removeError {
            alert.alertStyle = .warning
            alert.messageText = "アンインストールに失敗しました"
            alert.informativeText = "\(removeError.localizedDescription)\n\n手動で"
                + " ~/Library/Input Methods/iroha.app を削除してください。"
        } else {
            alert.messageText = "irohaをアンインストールしました"
            alert.informativeText = (purgeData
                ? "アプリとデータ（ユーザ辞書・学習・変換モデル・設定）を削除しました。"
                : "アプリを削除しました。ユーザ辞書・学習・変換モデル・設定は"
                    + " ~/Library/Application Support/iroha に残っています。")
                + "\n\n「システム設定 > プライバシーとセキュリティ > アクセシビリティ」の"
                + " iroha の項目は、必要に応じて手動で削除してください。"
        }
        alert.window.level = .modalPanel  // LSBackgroundOnlyでも前面に出す
        NSApp.activate(ignoringOtherApps: true)
        alert.runModal()
        _exit(0)
    }

    /// ABC（英数）キーボードを選択して、irohaが選択中でない状態にする
    private static func selectABCKeyboard() {
        let filter = [kTISPropertyInputSourceID as String: "com.apple.keylayout.ABC"]
        guard let list = TISCreateInputSourceList(filter as CFDictionary, true)?
            .takeRetainedValue() as? [TISInputSource],
            let abc = list.first else { return }
        TISEnableInputSource(abc)  // 無効化されていた場合に備える
        TISSelectInputSource(abc)
    }

    /// irohaの入力ソース（日本語・英字）をすべて無効化する。
    /// システム設定 > キーボード > 入力ソース から「−」で外すのと同じ効果
    private static func disableIrohaInputSources() {
        guard let bundleId = Bundle.main.bundleIdentifier else { return }
        let filter = [kTISPropertyBundleID as String: bundleId]
        guard let list = TISCreateInputSourceList(filter as CFDictionary, true)?
            .takeRetainedValue() as? [TISInputSource] else { return }
        for source in list {
            TISDisableInputSource(source)
        }
    }
}
