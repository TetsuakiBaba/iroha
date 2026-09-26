import Foundation

/// 配布している打ち間違い訂正モデルの一覧（`models/typo-normalizer.json`）。
///
/// アプリに焼き込むのは**カタログのURLだけ**で、モデルの追加・差し替え・URL変更は
/// カタログを更新するだけで済ませる（アプリの更新を待たせない）。重み本体は
/// GitHub Releases の専用タグに置く（`macos/scripts/publish-typo-normalizer.sh`）
public struct TypoNormalizerCatalog: Decodable, Sendable {

    /// 1ファイルぶんの取得先と検証情報
    public struct File: Decodable, Sendable {
        public let url: URL
        public let bytes: Int
        /// 16進小文字の SHA-256。落としたあと必ず照合する
        public let sha256: String
    }

    /// 学習元の1件（設定 > 情報 のライセンス一覧に1行で出す）
    public struct Source: Codable, Sendable, Equatable {
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

    public struct Model: Decodable, Sendable, Identifiable, Equatable {
        public let id: String
        /// 設定画面に出す名前
        public let name: String
        /// 補足（パラメータ数・速度など）
        public let summary: String?
        public let manifest: File
        public let weights: File
        /// このモデルの重みのライセンスと学習元。**アプリに焼き込まない**:
        /// 学習元コーパスによって条件が変わる（zenz-v2.5-dataset 由来なら CC BY-SA 4.0、
        /// iroha-dataset から作り直したものは別になりうる）ので、カタログ側に持たせる
        public let license: String?
        public let attribution: String?
        /// 学習元を1件ずつ分けたもの（`attribution` は1本の文章なので一覧に出せない）。
        /// 無いカタログ（古い世代）もあるので省略可
        public let sources: [Source]?
        /// モデルの配布ページ（ライセンス一覧のリンク先）
        public let page: String?

        public var totalBytes: Int { manifest.bytes + weights.bytes }

        public static func == (lhs: Model, rhs: Model) -> Bool { lhs.id == rhs.id }
    }

    /// カタログの形式。読めない世代が来たら諦めて既定の挙動に戻すために見る
    public let formatVersion: Int
    public let models: [Model]
    /// モデル側に書かれていないときの既定値
    public let license: String?
    public let attribution: String?

    /// 表示用のライセンス文（モデル固有 → カタログ既定 の順に見る）
    public func license(for model: Model) -> String? { model.license ?? license }
    public func attribution(for model: Model) -> String? { model.attribution ?? attribution }

    public static let supportedFormatVersion = 1

    /// カタログの場所。`IROHA_TYPO_CATALOG` で差し替えられる（開発・検証用）
    public static var defaultURL: URL {
        if let override = ProcessInfo.processInfo.environment["IROHA_TYPO_CATALOG"],
           !override.isEmpty, let url = URL(string: override) {
            return url
        }
        return URL(string:
            "https://raw.githubusercontent.com/TetsuakiBaba/iroha/main/models/typo-normalizer.json")!
    }

    public func model(id: String) -> Model? { models.first { $0.id == id } }
}

/// 取得したモデルの置き場所と、入っているかどうかの判定。
///
/// `<データフォルダ>/models/typo-normalizer/` に置く。zenz の GGUF と同じ `models/` の下なので、
/// 保存場所を共有フォルダ（Dropbox 等）にしている人は**1回落とせば全部のMacで使える**
public enum TypoNormalizerInstall {

    /// 入れたモデルの素性。更新の判定と設定画面の表示に使う
    public struct Record: Codable, Sendable, Equatable {
        public let id: String
        public let name: String
        /// weights.bin の SHA-256。カタログと違っていれば入れ直す
        public let sha256: String
        public let installedAt: Date
        /// 入れた時点のカタログにあったライセンス・学習元・配布ページ。カタログが取れない
        /// （オフライン）ときでも設定 > 情報 に出せるように残す。この項目より前に入れたものは nil
        public let license: String?
        public let sources: [TypoNormalizerCatalog.Source]?
        public let page: String?

        public init(id: String, name: String, sha256: String, installedAt: Date = Date(),
                    license: String? = nil, sources: [TypoNormalizerCatalog.Source]? = nil,
                    page: String? = nil) {
            self.id = id
            self.name = name
            self.sha256 = sha256
            self.installedAt = installedAt
            self.license = license
            self.sources = sources
            self.page = page
        }
    }

    public static var directoryURL: URL {
        DataDirectory.modelsURL.appendingPathComponent("typo-normalizer", isDirectory: true)
    }

    public static var recordURL: URL {
        directoryURL.appendingPathComponent("installed.json")
    }

    /// 使える状態で入っているか（manifest と weights が揃っているか）
    public static var isInstalled: Bool {
        TypoNormalizer.isModelDirectory(directoryURL)
    }

    /// 入っているモデルの素性（無ければ nil）
    public static func installedRecord() -> Record? {
        guard let data = try? Data(contentsOf: recordURL) else { return nil }
        return try? JSONDecoder.typoNormalizer.decode(Record.self, from: data)
    }

    public static func writeRecord(_ record: Record) throws {
        try FileManager.default.createDirectory(at: directoryURL, withIntermediateDirectories: true)
        try JSONEncoder.typoNormalizer.encode(record).write(to: recordURL, options: .atomic)
    }

    /// 入れたモデルを消す（設定画面の「削除」・アンインストール）
    public static func remove() throws {
        guard FileManager.default.fileExists(atPath: directoryURL.path) else { return }
        try FileManager.default.removeItem(at: directoryURL)
    }

    /// カタログの中身と入っているものを見比べて、入れ直しが要るか
    public static func needsUpdate(to model: TypoNormalizerCatalog.Model) -> Bool {
        guard isInstalled, let record = installedRecord() else { return true }
        return record.id != model.id || record.sha256.lowercased() != model.weights.sha256.lowercased()
    }
}

extension JSONDecoder {
    static let typoNormalizer: JSONDecoder = {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return decoder
    }()
}

extension JSONEncoder {
    static let typoNormalizer: JSONEncoder = {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return encoder
    }()
}
