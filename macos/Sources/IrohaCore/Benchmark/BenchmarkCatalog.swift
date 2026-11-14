import Foundation

/// 配布している評価セットの一覧（`models/benchmarks.json`）。
///
/// 専門辞書（`TermDictionaryCatalog`）と同じく、アプリに焼き込むのは**カタログの URL だけ**にする。
/// 評価セットの本体は GitHub Releases のプレリリースのタグ `benchmarks-v1` に置く
/// （`macos/scripts/publish-benchmarks.sh`）。iroha-bench-v1 は CC BY-SA 4.0 で、項目によっては CC BY 4.0・MIT の
/// 出典も混ざるので、本体のコード（MIT）とは分けて配り、ライセンスと出典はカタログに持たせる。
/// **評価セットの中身を変えたら ID を変える**（同じ ID の結果どうしを比べるため）
public struct BenchmarkCatalog: Codable, Sendable {

    public struct File: Codable, Sendable, Equatable {
        public let url: URL
        public let bytes: Int
        /// 16進小文字の SHA-256。落としたあと必ず照合する
        public let sha256: String

        public init(url: URL, bytes: Int, sha256: String) {
            self.url = url
            self.bytes = bytes
            self.sha256 = sha256
        }
    }

    /// 出典の1件（設定 > 情報 のライセンス一覧に1行で出す）
    public struct Source: Codable, Sendable, Equatable, Hashable {
        public let name: String
        public let holder: String
        public let license: String
        public let url: String

        public init(name: String, holder: String, license: String, url: String) {
            self.name = name
            self.holder = holder
            self.license = license
            self.url = url
        }
    }

    public struct Entry: Codable, Sendable, Identifiable, Equatable {
        public let id: String
        /// 設定画面に出す名前
        public let name: String
        /// 中身の説明（「?」に出す）
        public let summary: String?
        /// 項目の数
        public let items: Int
        public let file: File
        /// 評価セット全体のライセンス
        public let license: String
        public let sources: [Source]
        /// 出典の条文が写しに添えることを求める注記（MIT の著作権表示と許諾文など）。入れたときに素性へ写し、
        /// 評価セットのファイルの隣に残す（JSON Lines には注記の行を書けないため）
        public let notice: String?

        public init(id: String, name: String, summary: String?, items: Int, file: File,
                    license: String, sources: [Source], notice: String? = nil) {
            self.id = id
            self.name = name
            self.summary = summary
            self.items = items
            self.file = file
            self.license = license
            self.sources = sources
            self.notice = notice
        }
    }

    public let formatVersion: Int
    public let benchmarks: [Entry]

    public static let supportedFormatVersion = 1

    /// カタログの場所。`IROHA_BENCHMARK_CATALOG` で差し替えられる（開発・検証用。file:// も可）
    public static var defaultURL: URL {
        if let override = ProcessInfo.processInfo.environment["IROHA_BENCHMARK_CATALOG"],
           !override.isEmpty, let url = URL(string: override) {
            return url
        }
        return URL(string: "https://raw.githubusercontent.com/TetsuakiBaba/iroha/main/models/benchmarks.json")!
    }

    public func entry(id: String) -> Entry? { benchmarks.first { $0.id == id } }
}

/// 入れた評価セットの置き場所と素性。
///
/// `<データフォルダ>/benchmarks/<ID>.jsonl` に評価セット、`<ID>.json` に素性（カタログの項目と入れた日時）を置く
public enum BenchmarkInstall {

    public struct Record: Codable, Sendable, Equatable, Identifiable {
        public let id: String
        public let name: String
        public let items: Int
        public let sha256: String
        /// 入れた時点のカタログにあったライセンスと出典。オフラインでも 設定 > 情報 に出せるように残す
        public let license: String
        public let sources: [BenchmarkCatalog.Source]
        public let notice: String?
        public let installedAt: Date

        public init(entry: BenchmarkCatalog.Entry, installedAt: Date = Date()) {
            self.id = entry.id
            self.name = entry.name
            self.items = entry.items
            self.sha256 = entry.file.sha256.lowercased()
            self.license = entry.license
            self.sources = entry.sources
            self.notice = entry.notice
            self.installedAt = installedAt
        }
    }

    public static var directoryURL: URL {
        DataDirectory.url.appendingPathComponent("benchmarks", isDirectory: true)
    }

    public static func fileURL(id: String) -> URL {
        directoryURL.appendingPathComponent("\(id).jsonl")
    }

    public static func recordURL(id: String) -> URL {
        directoryURL.appendingPathComponent("\(id).json")
    }

    /// ID はファイル名に使うので、英小文字・数字・ハイフンだけを受け付ける
    public static func isValidID(_ id: String) -> Bool {
        TermDictionaryInstall.isValidID(id)
    }

    /// 入れている評価セット（ファイルと素性が揃っているもの）。ID の順
    public static func installedRecords() -> [Record] {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: directoryURL.path) else { return [] }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return names
            .filter { $0.hasSuffix(".json") }
            .compactMap { name -> Record? in
                let id = String(name.dropLast(5))
                guard isValidID(id),
                      FileManager.default.fileExists(atPath: fileURL(id: id).path),
                      let data = try? Data(contentsOf: recordURL(id: id)),
                      let record = try? decoder.decode(Record.self, from: data),
                      record.id == id
                else { return nil }
                return record
            }
            .sorted { $0.id < $1.id }
    }

    public static func writeRecord(_ record: Record) throws {
        try FileManager.default.createDirectory(at: directoryURL, withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(record).write(to: recordURL(id: record.id), options: .atomic)
    }

    public static func remove(id: String) throws {
        guard isValidID(id) else { return }
        for url in [fileURL(id: id), recordURL(id: id)] where FileManager.default.fileExists(atPath: url.path) {
            try FileManager.default.removeItem(at: url)
        }
    }
}

/// 評価セットの取得・照合・設置（`TermDictionaryFetcher` と同じ手順）
public enum BenchmarkFetcher {

    public enum FetchError: LocalizedError, Equatable {
        case unsupportedCatalog(Int)
        case invalidID(String)
        case http(status: Int, fileName: String)
        case sizeMismatch(expected: Int, actual: Int)
        case checksumMismatch

        public var errorDescription: String? {
            switch self {
            case .unsupportedCatalog(let version):
                return "評価セットの一覧の形式が新しすぎます（v\(version)）。irohaを更新してください"
            case .invalidID(let id):
                return "評価セットの一覧の ID が正しくありません: \(id)"
            case .http(let status, let fileName):
                return "取得できませんでした（HTTP \(status)）: \(fileName)"
            case .sizeMismatch(let expected, let actual):
                return "評価セットの大きさが違います（期待 \(expected) バイト、実際 \(actual) バイト）"
            case .checksumMismatch:
                return "評価セットが壊れています（チェックサム不一致）。もう一度お試しください"
            }
        }
    }

    public static func fetchCatalog(
        from url: URL = BenchmarkCatalog.defaultURL,
        session: URLSession = .shared
    ) async throws -> BenchmarkCatalog {
        var request = URLRequest(url: url)
        request.cachePolicy = .reloadIgnoringLocalCacheData
        request.timeoutInterval = 15
        let (data, response) = try await session.data(for: request)
        try check(response, name: url.lastPathComponent)
        let catalog = try JSONDecoder().decode(BenchmarkCatalog.self, from: data)
        guard catalog.formatVersion <= BenchmarkCatalog.supportedFormatVersion else {
            throw FetchError.unsupportedCatalog(catalog.formatVersion)
        }
        return catalog
    }

    /// 評価セットを取得して設置する（入っていれば置き換える）
    @discardableResult
    public static func install(
        _ entry: BenchmarkCatalog.Entry,
        session: URLSession = .shared
    ) async throws -> BenchmarkInstall.Record {
        guard BenchmarkInstall.isValidID(entry.id) else { throw FetchError.invalidID(entry.id) }
        var request = URLRequest(url: entry.file.url)
        request.timeoutInterval = 60
        let (temporary, response) = try await session.download(for: request)
        defer { try? FileManager.default.removeItem(at: temporary) }
        try check(response, name: entry.file.url.lastPathComponent)
        try Task.checkCancellation()

        let size = (try FileManager.default.attributesOfItem(atPath: temporary.path)[.size] as? Int) ?? -1
        guard size == entry.file.bytes else {
            throw FetchError.sizeMismatch(expected: entry.file.bytes, actual: size)
        }
        guard try SHA256.hex(contentsOf: temporary).caseInsensitiveCompare(entry.file.sha256) == .orderedSame
        else { throw FetchError.checksumMismatch }

        try FileManager.default.createDirectory(at: BenchmarkInstall.directoryURL, withIntermediateDirectories: true)
        let destination = BenchmarkInstall.fileURL(id: entry.id)
        if FileManager.default.fileExists(atPath: destination.path) {
            _ = try FileManager.default.replaceItemAt(destination, withItemAt: temporary)
        } else {
            try FileManager.default.moveItem(at: temporary, to: destination)
        }
        let record = BenchmarkInstall.Record(entry: entry)
        try BenchmarkInstall.writeRecord(record)
        return record
    }

    private static func check(_ response: URLResponse, name: String) throws {
        guard let http = response as? HTTPURLResponse else { return }
        guard (200..<300).contains(http.statusCode) else {
            throw FetchError.http(status: http.statusCode, fileName: name)
        }
    }
}
