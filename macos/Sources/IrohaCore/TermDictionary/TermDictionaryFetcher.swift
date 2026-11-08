import Foundation

/// 専門辞書の取得・照合・設置。
///
/// UI に依存しないので `IrohaCore` に置く（進捗と状態の表示は macOS の `TermDictionaryDownloader`）。
/// 壊れた辞書を置かないため、一時フォルダへ落として**大きさと SHA-256 を照合してから**設置先へ移す
public enum TermDictionaryFetcher {

    public enum FetchError: LocalizedError, Equatable {
        case unsupportedCatalog(Int)
        case invalidID(String)
        case http(status: Int, fileName: String)
        case sizeMismatch(expected: Int, actual: Int)
        case checksumMismatch

        public var errorDescription: String? {
            switch self {
            case .unsupportedCatalog(let version):
                return "辞書一覧の形式が新しすぎます（v\(version)）。irohaを更新してください"
            case .invalidID(let id):
                return "辞書一覧の ID が正しくありません: \(id)"
            case .http(let status, let fileName):
                return "取得できませんでした（HTTP \(status)）: \(fileName)"
            case .sizeMismatch(let expected, let actual):
                return "辞書の大きさが違います（期待 \(expected) バイト、実際 \(actual) バイト）"
            case .checksumMismatch:
                return "辞書が壊れています（チェックサム不一致）。もう一度お試しください"
            }
        }
    }

    /// 辞書の一覧を取りにいく
    public static func fetchCatalog(
        from url: URL = TermDictionaryCatalog.defaultURL,
        session: URLSession = .shared
    ) async throws -> TermDictionaryCatalog {
        var request = URLRequest(url: url)
        request.cachePolicy = .reloadIgnoringLocalCacheData
        request.timeoutInterval = 15
        let (data, response) = try await session.data(for: request)
        try check(response, name: url.lastPathComponent)
        let catalog = try JSONDecoder().decode(TermDictionaryCatalog.self, from: data)
        guard catalog.formatVersion <= TermDictionaryCatalog.supportedFormatVersion else {
            throw FetchError.unsupportedCatalog(catalog.formatVersion)
        }
        return catalog
    }

    /// 辞書を取得して設置する（入っていれば置き換える）。`onProgress` は 0.0〜1.0
    @discardableResult
    public static func install(
        _ dictionary: TermDictionaryCatalog.Dictionary,
        session: URLSession = .shared,
        onProgress: @escaping @Sendable (Double) -> Void = { _ in }
    ) async throws -> TermDictionaryInstall.Record {
        guard TermDictionaryInstall.isValidID(dictionary.id) else {
            throw FetchError.invalidID(dictionary.id)
        }
        onProgress(0)
        var request = URLRequest(url: dictionary.file.url)
        request.timeoutInterval = 60
        let (temporary, response) = try await session.download(
            for: request, delegate: ProgressDelegate(onProgress: onProgress))
        defer { try? FileManager.default.removeItem(at: temporary) }
        try check(response, name: dictionary.file.url.lastPathComponent)
        try Task.checkCancellation()

        let size = (try FileManager.default.attributesOfItem(atPath: temporary.path)[.size] as? Int) ?? -1
        guard size == dictionary.file.bytes else {
            throw FetchError.sizeMismatch(expected: dictionary.file.bytes, actual: size)
        }
        guard try SHA256.hex(contentsOf: temporary).caseInsensitiveCompare(dictionary.file.sha256) == .orderedSame
        else { throw FetchError.checksumMismatch }

        try FileManager.default.createDirectory(
            at: TermDictionaryInstall.directoryURL, withIntermediateDirectories: true)
        let destination = TermDictionaryInstall.fileURL(id: dictionary.id)
        if FileManager.default.fileExists(atPath: destination.path) {
            _ = try FileManager.default.replaceItemAt(destination, withItemAt: temporary)
        } else {
            try FileManager.default.moveItem(at: temporary, to: destination)
        }
        let record = TermDictionaryInstall.Record(dictionary: dictionary)
        try TermDictionaryInstall.writeRecord(record)
        onProgress(1)
        return record
    }

    // MARK: - 部品

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
