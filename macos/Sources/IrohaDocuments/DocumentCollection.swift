import Foundation
import IrohaCore

/// フォルダに置いた文書をまとめて読み、文と語を取り出す。
///
/// 既定のフォルダは `<データフォルダ>/documents/`（`DocumentCollection.defaultFolderURL`）。
/// 文書そのものは iroha のどこにもコピーしない
public struct DocumentCollection: Sendable {

    /// 1 つの文書から取り出したもの
    public struct Document: Sendable {
        /// ファイル名
        public let name: String
        /// 整えた本文（参考文献の節は除く）
        public let text: String
        /// 文ごとの分かち書き
        public let sentences: [[ReadingToken]]
        /// この文書に出てきた語の候補（出現ごと）
        public let terms: [DocumentTermOccurrence]
        /// この文書で数えた表記の好み（読みごとの表記の回数）
        public let notation: NotationPreference
    }

    public let documents: [Document]
    /// 読めなかったファイルと理由
    public let failures: [(name: String, reason: String)]

    public static var defaultFolderURL: URL {
        DataDirectory.url.appendingPathComponent("documents", isDirectory: true)
    }

    /// フォルダ直下と下位フォルダの対応する文書をすべて読む（ファイル名の順）
    public init(folder: URL, tokenizer: any ReadingTokenizer = SystemReadingTokenizer()) {
        var documents: [Document] = []
        var failures: [(String, String)] = []
        for url in Self.documentURLs(in: folder) {
            let name = url.path.replacingOccurrences(of: folder.standardizedFileURL.path + "/", with: "")
            do {
                let raw = try DocumentTextReader.text(of: url)
                documents.append(Self.makeDocument(name: name, raw: raw, tokenizer: tokenizer))
            } catch {
                failures.append((name, "\(error)"))
            }
        }
        self.documents = documents
        self.failures = failures
    }

    public static func makeDocument(name: String, raw: String, tokenizer: any ReadingTokenizer) -> Document {
        let text = DocumentText.removingReferences(DocumentText.normalize(raw))
        let sentences = DocumentText.sentences(in: text).map { tokenizer.tokens(in: $0) }
        let terms = sentences.flatMap { DocumentTermExtractor.terms(in: $0) }
        return Document(
            name: name, text: text, sentences: sentences, terms: terms,
            notation: NotationPreference(sentences: sentences))
    }

    /// すべての文書から集めた語の一覧
    public var vocabulary: DocumentVocabulary {
        DocumentVocabulary(occurrences: documents.map { ($0.name, $0.terms) })
    }

    /// すべての文書で数えた表記の好み
    public var notation: NotationPreference {
        documents.reduce(NotationPreference.empty) { $0.adding($1.notation) }
    }

    static func documentURLs(in folder: URL) -> [URL] {
        guard let enumerator = FileManager.default.enumerator(
            at: folder, includingPropertiesForKeys: [.isRegularFileKey],
            options: [.skipsHiddenFiles, .skipsPackageDescendants])
        else { return [] }
        var urls: [URL] = []
        for case let url as URL in enumerator where DocumentTextReader.isSupported(url) {
            urls.append(url.standardizedFileURL)
        }
        return urls.sorted { $0.path < $1.path }
    }
}
