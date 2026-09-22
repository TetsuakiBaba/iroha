import Foundation

/// 打ち間違い訂正モデルの取得・検証・設置。
///
/// UI に依存しないので `IrohaCore` に置く（Windows 版でもそのまま要る処理）。
/// 進捗と状態の発行は各プラットフォームの薄い層に任せる（macOS は `TypoNormalizerDownloader`）。
///
/// 壊れたモデルを絶対に置かないため、**一時ディレクトリへ落として SHA-256 と大きさを
/// 照合してから**設置先へ移す。途中で失敗しても既に入っているものは壊れない
public enum TypoNormalizerFetcher {

    public enum FetchError: LocalizedError, Equatable {
        case noModels
        case unsupportedCatalog(Int)
        case http(status: Int, fileName: String)
        case sizeMismatch(name: String, expected: Int, actual: Int)
        case checksumMismatch(name: String)

        public var errorDescription: String? {
            switch self {
            case .noModels:
                return "配布中のモデルが見つかりません"
            case .unsupportedCatalog(let version):
                return "モデル一覧の形式が新しすぎます（v\(version)）。irohaを更新してください"
            case .http(let status, let fileName):
                return "取得できませんでした（HTTP \(status)）: \(fileName)"
            case .sizeMismatch(let name, let expected, let actual):
                return "\(name) の大きさが違います（期待 \(expected) バイト、実際 \(actual) バイト）"
            case .checksumMismatch(let name):
                return "\(name) が壊れています（チェックサム不一致）。もう一度お試しください"
            }
        }
    }

    /// モデル一覧を取りにいく
    public static func fetchCatalog(
        from url: URL = TypoNormalizerCatalog.defaultURL,
        session: URLSession = .shared
    ) async throws -> TypoNormalizerCatalog {
        var request = URLRequest(url: url)
        request.cachePolicy = .reloadIgnoringLocalCacheData
        request.timeoutInterval = 15
        let (data, response) = try await session.data(for: request)
        try check(response, name: url.lastPathComponent)
        let catalog = try JSONDecoder().decode(TypoNormalizerCatalog.self, from: data)
        guard catalog.formatVersion <= TypoNormalizerCatalog.supportedFormatVersion else {
            throw FetchError.unsupportedCatalog(catalog.formatVersion)
        }
        guard !catalog.models.isEmpty else { throw FetchError.noModels }
        return catalog
    }

    /// モデルを取得して設置する。`onProgress` は 0.0〜1.0（2ファイルを大きさの比で合成したもの）
    @discardableResult
    public static func install(
        _ model: TypoNormalizerCatalog.Model,
        session: URLSession = .shared,
        onProgress: @escaping @Sendable (Double) -> Void = { _ in }
    ) async throws -> TypoNormalizerInstall.Record {
        let staging = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-typo-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: staging, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: staging) }

        let files = [(model.manifest, "manifest.json"), (model.weights, "weights.bin")]
        let total = Double(max(1, model.totalBytes))
        var completed = 0.0
        onProgress(0)

        for (file, name) in files {
            let base = completed
            try await download(file, to: staging.appendingPathComponent(name), session: session) {
                onProgress(min(1, (base + $0 * Double(file.bytes)) / total))
            }
            completed += Double(file.bytes)
            try Task.checkCancellation()
        }

        for (file, name) in files {
            let url = staging.appendingPathComponent(name)
            let size = (try FileManager.default.attributesOfItem(atPath: url.path)[.size] as? Int) ?? -1
            guard size == file.bytes else {
                throw FetchError.sizeMismatch(name: name, expected: file.bytes, actual: size)
            }
            guard try SHA256.hex(contentsOf: url).caseInsensitiveCompare(file.sha256) == .orderedSame
            else {
                throw FetchError.checksumMismatch(name: name)
            }
        }

        // 照合に通ったので入れ替える。古いものを消してから移すので中途半端な組み合わせが残らない
        let directory = TypoNormalizerInstall.directoryURL
        try? FileManager.default.removeItem(at: directory)
        try FileManager.default.createDirectory(
            at: directory.deletingLastPathComponent(), withIntermediateDirectories: true)
        try FileManager.default.moveItem(at: staging, to: directory)
        let record = TypoNormalizerInstall.Record(
            id: model.id, name: model.name, sha256: model.weights.sha256)
        try TypoNormalizerInstall.writeRecord(record)
        onProgress(1)
        return record
    }

    // MARK: - 部品

    private static func download(
        _ file: TypoNormalizerCatalog.File, to destination: URL, session: URLSession,
        onProgress: @escaping @Sendable (Double) -> Void
    ) async throws {
        var request = URLRequest(url: file.url)
        request.timeoutInterval = 60
        let delegate = ProgressDelegate(onProgress: onProgress)
        let (temporary, response) = try await session.download(for: request, delegate: delegate)
        try check(response, name: destination.lastPathComponent)
        try? FileManager.default.removeItem(at: destination)
        try FileManager.default.moveItem(at: temporary, to: destination)
    }

    private static func check(_ response: URLResponse, name: String) throws {
        // file:// では HTTPURLResponse にならない（検証用・開発用に使う）
        guard let http = response as? HTTPURLResponse else { return }
        guard (200..<300).contains(http.statusCode) else {
            throw FetchError.http(status: http.statusCode, fileName: name)
        }
    }

    /// `URLSession.download(for:delegate:)` に進捗を足すためだけの入れ物
    private final class ProgressDelegate: NSObject, URLSessionTaskDelegate, URLSessionDownloadDelegate {
        private let onProgress: @Sendable (Double) -> Void

        init(onProgress: @escaping @Sendable (Double) -> Void) {
            self.onProgress = onProgress
        }

        func urlSession(
            _ session: URLSession, downloadTask: URLSessionDownloadTask,
            didWriteData bytesWritten: Int64, totalBytesWritten: Int64,
            totalBytesExpectedToWrite: Int64
        ) {
            guard totalBytesExpectedToWrite > 0 else { return }
            onProgress(Double(totalBytesWritten) / Double(totalBytesExpectedToWrite))
        }

        func urlSession(
            _ session: URLSession, downloadTask: URLSessionDownloadTask,
            didFinishDownloadingTo location: URL
        ) {}
    }
}
