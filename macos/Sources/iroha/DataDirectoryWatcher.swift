import Foundation
import IrohaCore

/// データフォルダを監視し、他のMacからの同期（iCloud Drive / Dropbox）で
/// ファイルが変わったらストアと設定を読み直す。
///
/// フォルダのvnodeイベント（同期クライアントは一時ファイルを書いてrenameするので
/// フォルダの.writeとして観測できる）に加え、その場で書き換えられた場合の保険として
/// 一定間隔でも更新日時を確かめる。自分の保存も同じイベントを起こすが、各ストアが
/// 保存時の更新日時を覚えているので空振りするだけ
final class DataDirectoryWatcher {

    static let shared = DataDirectoryWatcher()

    private var source: DispatchSourceFileSystemObject?
    private var descriptor: Int32 = -1
    private var pending: DispatchWorkItem?
    private var timer: DispatchSourceTimer?
    private let queue = DispatchQueue(label: "iroha.datadir.watch", qos: .utility)

    /// 定期確認の間隔（秒）
    private let pollInterval: TimeInterval = 60

    private init() {}

    func start() {
        guard source == nil else { return }
        let url = DataDirectory.url
        descriptor = open(url.path, O_EVTONLY)
        if descriptor >= 0 {
            let source = DispatchSource.makeFileSystemObjectSource(
                fileDescriptor: descriptor, eventMask: [.write, .rename, .delete, .link], queue: queue)
            source.setEventHandler { [weak self] in self?.scheduleCheck() }
            source.setCancelHandler { [descriptor = self.descriptor] in close(descriptor) }
            source.resume()
            self.source = source
        } else {
            NSLog("iroha: データフォルダの監視を開始できません: \(url.path)")
        }

        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + pollInterval, repeating: pollInterval)
        timer.setEventHandler { [weak self] in self?.check() }
        timer.resume()
        self.timer = timer
    }

    /// 同期クライアントは複数ファイルを続けて書くのでまとめて処理する
    private func scheduleCheck() {
        pending?.cancel()
        let task = DispatchWorkItem { [weak self] in self?.check() }
        pending = task
        queue.asyncAfter(deadline: .now() + 0.5, execute: task)
    }

    private func check() {
        var reloaded: [String] = []
        if UserDictionaryStore.shared.reloadIfChanged() { reloaded.append("ユーザ辞書") }
        if LearningStore.shared.reloadIfChanged() { reloaded.append("学習") }
        if UserRewriteRuleStore.shared.reloadIfChanged() { reloaded.append("変換ルール") }
        DispatchQueue.main.async {
            if PreferencesSync.shared.importIfNewer() { reloaded.append("設定") }
            if !reloaded.isEmpty {
                NSLog("iroha: データフォルダの変更を反映: \(reloaded.joined(separator: "・"))")
            }
        }
    }
}
