import Foundation
import IrohaCore

/// 変換モデル(zenz GGUF)の初回自動ダウンロード。
/// モデルが無くてもかな入力は動作し、ダウンロード完了後は再起動不要で
/// 変換が始まる（ZenzEngineは変換のたびにロードを再試行するため）。
final class ModelDownloader: NSObject, ObservableObject {
    static let shared = ModelDownloader()

    /// scripts/fetch-model.sh と同じ配布元（CC-BY-SA-4.0, Keita Miwa氏）
    private static let modelURL = URL(string:
        "https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf/resolve/main/ggml-model-Q5_K_M.gguf")!

    enum State: Equatable {
        case idle
        case downloading(progress: Double)  // 0.0-1.0
        case done
        case failed(String)
    }

    @Published private(set) var state: State = .idle

    private var session: URLSession?

    /// モデルが未設定・未取得ならバックグラウンドでダウンロードを開始する
    func startIfNeeded() {
        // ユーザーが独自モデルを指定している場合は何もしない
        if let custom = UserDefaults.standard.string(forKey: "modelPath"), !custom.isEmpty { return }
        guard !FileManager.default.fileExists(atPath: ZenzEngine.defaultModelPath) else { return }
        if case .downloading = state { return }

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
