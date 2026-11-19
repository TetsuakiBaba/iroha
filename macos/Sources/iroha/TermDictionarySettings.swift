import Foundation
import IrohaCore

/// 専門辞書の設定（設定 > 辞書・学習 > 専門辞書）。
///
/// 「ライブ変換にも使う」にした辞書の ID を、カンマ区切りの文字列で持つ（既定は空・同期する。
/// 入れた辞書はデータフォルダにあって他の Mac と共有されるので、選び方も合わせる）。
/// 使う辞書の語は `TermDictionaryLiveEngine` がユーザ辞書と同じ規則で読みに当てる
enum TermDictionarySettings {

    static let liveConversionKey = "termDictionaryLiveConversion"

    static var liveConversionIDs: Set<String> {
        ids(from: UserDefaults.standard.string(forKey: liveConversionKey) ?? "")
    }

    static func ids(from text: String) -> Set<String> {
        Set(text.split(separator: ",").map(String.init).filter { !$0.isEmpty })
    }

    static func text(from ids: Set<String>) -> String {
        ids.sorted().joined(separator: ",")
    }

    /// 変換に使う、ライブ変換にも使う辞書。プライベートモードの間はユーザ辞書と同じく使わない
    /// （モードの間は変換を iroha 本来の結果にする）
    static var liveLookup: TermDictionaryLookup {
        PrivateModeSettings.isEnabled ? .empty : TermDictionaryStore.shared.liveLookup
    }

    /// 起動時に、ストアへ設定の読み方を渡す。設定が変わったら（他の Mac からの同期を含む）読み直させる
    static func start() {
        TermDictionaryStore.shared.setLiveConversionIDs { liveConversionIDs }
        var last = liveConversionIDs
        NotificationCenter.default.addObserver(
            forName: UserDefaults.didChangeNotification, object: nil, queue: .main
        ) { _ in
            let now = liveConversionIDs
            guard now != last else { return }
            last = now
            TermDictionaryStore.shared.reloadInBackground()
        }
    }
}
