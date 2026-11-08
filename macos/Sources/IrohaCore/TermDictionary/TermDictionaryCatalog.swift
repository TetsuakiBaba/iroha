import Foundation

/// 配布している専門辞書の一覧（`models/term-dictionaries.json`）。
///
/// アプリに焼き込むのは**カタログの URL だけ**で、辞書の追加・作り直し・URL の変更はカタログを
/// 更新するだけで済ませる。辞書の本体は GitHub Releases のプレリリースのタグ `term-dictionaries-v1` に置く
/// （`macos/scripts/publish-term-dictionaries.sh`）。辞書ごとにライセンスが違う（JMdict は CC BY-SA 4.0、
/// 百薬辞書は CC BY 4.0、Wikidata は CC0 など）ので、ライセンスと出典もカタログに持たせる
public struct TermDictionaryCatalog: Codable, Sendable {

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
        /// 作成者・著作権者（「© 」を前に付けて表示する）
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

    public struct Dictionary: Codable, Sendable, Identifiable, Equatable {
        public let id: String
        /// 設定画面に出す名前（「医学」「人名」）
        public let name: String
        /// 設定画面で見出しにする、辞書のまとまり（「分野別の用語（JMdict）」）
        public let group: String
        /// 中身の説明（「?」に出す）
        public let summary: String?
        /// 収録している「読み・語」の組の数
        public let entries: Int
        public let file: File
        /// 語の種類（辞書ラティスでの品詞）。無ければ用語として扱う
        public let kind: TermDictionary.Kind?
        public let license: String
        public let sources: [Source]
        /// 作った日（元データを取った日、YYYYMMDD）。入れている版と違えば入れ直す
        public let version: String

        public init(id: String, name: String, group: String, summary: String?, entries: Int,
                    file: File, kind: TermDictionary.Kind?, license: String, sources: [Source],
                    version: String) {
            self.id = id
            self.name = name
            self.group = group
            self.summary = summary
            self.entries = entries
            self.file = file
            self.kind = kind
            self.license = license
            self.sources = sources
            self.version = version
        }
    }

    /// カタログの形式。読めない世代が来たら取得を諦める
    public let formatVersion: Int
    public let dictionaries: [Dictionary]

    public static let supportedFormatVersion = 1

    /// カタログの場所。`IROHA_TERM_DICTIONARY_CATALOG` で差し替えられる（開発・検証用。file:// も可）
    public static var defaultURL: URL {
        if let override = ProcessInfo.processInfo.environment["IROHA_TERM_DICTIONARY_CATALOG"],
           !override.isEmpty, let url = URL(string: override) {
            return url
        }
        return URL(string:
            "https://raw.githubusercontent.com/TetsuakiBaba/iroha/main/models/term-dictionaries.json")!
    }

    public func dictionary(id: String) -> Dictionary? { dictionaries.first { $0.id == id } }

    /// 見出し（`group`）ごとに、カタログに出てきた順でまとめる
    public var groups: [(name: String, dictionaries: [Dictionary])] {
        var order: [String] = []
        var members: [String: [Dictionary]] = [:]
        for dictionary in dictionaries {
            if members[dictionary.group] == nil { order.append(dictionary.group) }
            members[dictionary.group, default: []].append(dictionary)
        }
        return order.map { ($0, members[$0] ?? []) }
    }
}

/// 入れた専門辞書の置き場所と素性。
///
/// `<データフォルダ>/dictionaries/<ID>.tsv` に辞書、`<ID>.json` に素性を置く。データフォルダを共有フォルダ
/// （Dropbox 等）にしている人は、1台で入れれば全部のMacで使える。素性を辞書ごとのファイルに分けるのは、
/// 2台で別々に入れ外ししたときに1つのファイルを書き合わないため
public enum TermDictionaryInstall {

    public struct Record: Codable, Sendable, Equatable, Identifiable {
        public let id: String
        public let name: String
        public let group: String
        public let version: String
        /// 辞書ファイルの SHA-256
        public let sha256: String
        public let entries: Int
        public let kind: TermDictionary.Kind
        /// 入れた時点のカタログにあったライセンスと出典。オフラインでも 設定 > 情報 に出せるように残す
        public let license: String
        public let sources: [TermDictionaryCatalog.Source]
        public let installedAt: Date

        public init(dictionary: TermDictionaryCatalog.Dictionary, installedAt: Date = Date()) {
            self.id = dictionary.id
            self.name = dictionary.name
            self.group = dictionary.group
            self.version = dictionary.version
            self.sha256 = dictionary.file.sha256.lowercased()
            self.entries = dictionary.entries
            self.kind = dictionary.kind ?? .noun
            self.license = dictionary.license
            self.sources = dictionary.sources
            self.installedAt = installedAt
        }
    }

    public static var directoryURL: URL {
        DataDirectory.url.appendingPathComponent("dictionaries", isDirectory: true)
    }

    public static func fileURL(id: String) -> URL {
        directoryURL.appendingPathComponent("\(id).tsv")
    }

    public static func recordURL(id: String) -> URL {
        directoryURL.appendingPathComponent("\(id).json")
    }

    /// ID はファイル名に使うので、英小文字・数字・ハイフンだけを受け付ける（カタログの値をパスに入れるため）
    public static func isValidID(_ id: String) -> Bool {
        !id.isEmpty && id.count <= 64
            && id.unicodeScalars.allSatisfy { ("a"..."z").contains($0) || ("0"..."9").contains($0) || $0 == "-" }
    }

    /// 入れている辞書（辞書ファイルと素性が揃っているもの）。名前の順
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

    /// 辞書を外す（素性を先に消すので、途中で失敗しても「入っている」とは見なされない）
    public static func remove(id: String) throws {
        guard isValidID(id) else { return }
        for url in [recordURL(id: id), fileURL(id: id)] where FileManager.default.fileExists(atPath: url.path) {
            try FileManager.default.removeItem(at: url)
        }
    }

    /// カタログの版と入れている版が違うか（作り直された辞書は入れ直す）
    public static func needsUpdate(_ record: Record, to dictionary: TermDictionaryCatalog.Dictionary) -> Bool {
        record.sha256 != dictionary.file.sha256.lowercased()
    }
}
