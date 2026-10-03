import Foundation
import IrohaCore

/// 既定の変換モデル（GGUF）の初回自動ダウンロード。
/// モデルが無くてもかな入力は動作し、ダウンロード完了後は再起動不要で
/// 変換が始まる（ZenzEngineは変換のたびにロードを再試行するため）。
final class ModelDownloader: NSObject, ObservableObject {
    static let shared = ModelDownloader()

    /// 既定のモデル iroha-t5-alpha（文字単位 T5、zenz-v2.5-dataset で学習、CC BY-SA 4.0）。
    /// 重みは本体コード(MIT)と別ライセンスなので、アプリのリリースとは別のタグ（プレリリース）に置く。
    /// scripts/fetch-model.sh と同じ配布元
    private static let modelURL = URL(string:
        "https://github.com/TetsuakiBaba/iroha/releases/download/kkc-model-v1/iroha-t5-alpha-Q8_0.gguf")!
    /// 取得したファイルの照合（途中で切れた・差し替わったファイルを置かない）
    private static let modelBytes: Int64 = 121_290_560
    private static let modelSHA256 = "262baeb22a1ce640dc435eab217137809bec935cc663ae91e65a0458e8e7599b"

    enum State: Equatable {
        case idle
        case downloading(progress: Double)  // 0.0-1.0
        case done
        case failed(String)
    }

    @Published private(set) var state: State = .idle

    private var session: URLSession?
    private var lastAttempt: Date?

    /// IMEメニューに出す取得状況（表示不要ならnil）
    var statusMenuText: String? {
        // 独自モデル指定時や取得済みなら表示しない
        if let custom = UserDefaults.standard.string(forKey: "modelPath"), !custom.isEmpty { return nil }
        if FileManager.default.fileExists(atPath: ZenzEngine.defaultModelPath) { return nil }
        switch state {
        case .downloading(let progress):
            return "変換モデルをダウンロード中… \(Int(progress * 100))%"
        case .failed:
            return "変換モデルのダウンロードに失敗（自動で再試行します）"
        default:
            return "変換モデルを準備中…"
        }
    }

    /// モデルが未設定・未取得ならバックグラウンドでダウンロードを開始する。
    /// 失敗後の再試行はactivateServerからも呼ばれるため60秒のスロットル付き
    func startIfNeeded() {
        // ユーザーが独自モデルを指定している場合は何もしない
        if let custom = UserDefaults.standard.string(forKey: "modelPath"), !custom.isEmpty { return }
        guard !FileManager.default.fileExists(atPath: ZenzEngine.defaultModelPath) else { return }
        if case .downloading = state { return }
        if let last = lastAttempt, Date().timeIntervalSince(last) < 60 { return }
        lastAttempt = Date()

        NSLog("iroha: 変換モデルをダウンロードします: \(Self.modelURL)")
        setState(.downloading(progress: 0))
        let session = URLSession(configuration: .default, delegate: self, delegateQueue: nil)
        self.session = session
        session.downloadTask(with: Self.modelURL).resume()
    }

    private func setState(_ newState: State) {
        DispatchQueue.main.async { self.state = newState }
    }
}

extension ModelDownloader: URLSessionDownloadDelegate {
    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didWriteData bytesWritten: Int64, totalBytesWritten: Int64,
                    totalBytesExpectedToWrite: Int64) {
        guard totalBytesExpectedToWrite > 0 else { return }
        setState(.downloading(progress: Double(totalBytesWritten) / Double(totalBytesExpectedToWrite)))
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didFinishDownloadingTo location: URL) {
        defer { session.finishTasksAndInvalidate() }
        guard (downloadTask.response as? HTTPURLResponse)?.statusCode == 200 else {
            setState(.failed("HTTP \((downloadTask.response as? HTTPURLResponse)?.statusCode ?? 0)"))
            return
        }
        do {
            let size = (try? FileManager.default.attributesOfItem(atPath: location.path)[.size] as? Int64) ?? -1
            guard size == Self.modelBytes,
                  try SHA256.hex(contentsOf: location).caseInsensitiveCompare(Self.modelSHA256) == .orderedSame
            else {
                NSLog("iroha: 変換モデルの照合に失敗しました（大きさ \(size)）")
                setState(.failed("ダウンロードしたファイルが壊れています"))
                return
            }
            // fetch-model.sh と同じく .tmp に置いてから rename（部分ファイルを残さない）
            let finalPath = ZenzEngine.defaultModelPath
            let dir = (finalPath as NSString).deletingLastPathComponent
            try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
            let tmpPath = finalPath + ".tmp"
            try? FileManager.default.removeItem(atPath: tmpPath)
            try FileManager.default.moveItem(atPath: location.path, toPath: tmpPath)
            try FileManager.default.moveItem(atPath: tmpPath, toPath: finalPath)
            NSLog("iroha: 変換モデルのダウンロード完了: \(finalPath)")
            setState(.done)
        } catch {
            NSLog("iroha: モデル保存エラー: \(error)")
            setState(.failed(error.localizedDescription))
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error {
            NSLog("iroha: モデルダウンロードエラー: \(error)")
            setState(.failed(error.localizedDescription))
            session.finishTasksAndInvalidate()
        }
        // 失敗しても次回起動時のstartIfNeededで再試行される
    }
}
