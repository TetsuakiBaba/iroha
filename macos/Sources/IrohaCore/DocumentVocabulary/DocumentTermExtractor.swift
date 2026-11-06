import Foundation

/// 分かち書きの 1 語（表記と読み）。読みはひらがな。読みが分からない語（英字・記号など）は nil
public struct ReadingToken: Sendable, Equatable {
    public let surface: String
    public let reading: String?

    public init(surface: String, reading: String?) {
        self.surface = surface
        self.reading = reading
    }
}

/// 文字列を分かち書きして読みを付ける。macOS では `CFStringTokenizer`（IrohaDocuments の
/// `SystemReadingTokenizer`）。IrohaCore は OS の機能に依存させないので、ここでは口だけを決める
public protocol ReadingTokenizer: Sendable {
    func tokens(in text: String) -> [ReadingToken]
}

/// 文書から集めた語の候補（1 か所の出現）
public struct DocumentTermOccurrence: Sendable, Equatable, Hashable {
    public let surface: String
    public let reading: String
}

/// 分かち書きした語の並びから、文書に固有の語（専門用語・複合語）の候補を取り出す。
///
/// 品詞は使わず、文字の種類で決める: ひらがなを含まない語（漢字・カタカナだけの語）が続く範囲を
/// 1 つの複合語の候補にする。分かち書きは複合語を細かく分ける（「加速/度/センサ」「再/採点」）ので、
/// 続く範囲の中の連続する部分もすべて候補にする（「視覚/障害/者」→ 視覚障害者・視覚障害・障害者 …）。
/// 1 語だけの候補は 2 文字以上のものに限る（1 文字の漢字は辞書に必ずあり、語として数える意味がない）。
///
/// 英字・数字を含む語は対象にしない。iroha では英字はかなと別の打ち方（Shift・区切り記号）で入れるので、
/// かなの読みに一致させる語の一覧には入らない
public enum DocumentTermExtractor {

    /// 1 つの候補にまとめる語の数の上限
    public static let maxTokensPerTerm = 6

    public static func terms(in tokens: [ReadingToken]) -> [DocumentTermOccurrence] {
        var result: [DocumentTermOccurrence] = []
        var run: [(surface: String, reading: String)] = []
        func flush() {
            defer { run.removeAll() }
            guard !run.isEmpty else { return }
            for start in run.indices {
                var surface = ""
                var reading = ""
                for end in start..<min(run.count, start + maxTokensPerTerm) {
                    surface += run[end].surface
                    reading += run[end].reading
                    let isSingle = end == start
                    if isSingle && surface.count < 2 { continue }
                    result.append(DocumentTermOccurrence(surface: surface, reading: reading))
                }
            }
        }
        for token in tokens {
            if let reading = token.reading, isContentToken(token.surface), !reading.isEmpty {
                run.append((token.surface, reading))
            } else {
                flush()
            }
        }
        flush()
        return result
    }

    /// 複合語の部品になる語か（漢字・カタカナ・「ー」「々」だけでできていて、漢字かカタカナを含む）
    static func isContentToken(_ surface: String) -> Bool {
        guard !surface.isEmpty else { return false }
        var hasContent = false
        for scalar in surface.unicodeScalars {
            if DocumentText.isKanji(scalar) || DocumentText.isKatakana(scalar) {
                if scalar.value != 0x30FC { hasContent = true }
            } else {
                return false
            }
        }
        return hasContent
    }
}

/// 文書から集めた語の一覧（語・読み・出現回数・出てきた文書）
public struct DocumentVocabulary: Codable, Sendable, Equatable {

    public struct Term: Codable, Sendable, Equatable {
        public let surface: String
        public let reading: String
        /// すべての文書を通した出現回数
        public var count: Int
        /// 出てきた文書（ファイル名）
        public var documents: [String]

        public init(surface: String, reading: String, count: Int, documents: [String]) {
            self.surface = surface
            self.reading = reading
            self.count = count
            self.documents = documents
        }
    }

    /// 出現回数の多い順（同数なら表記の長い順・表記順）
    public private(set) var terms: [Term]

    public static let empty = DocumentVocabulary(terms: [])

    public init(terms: [Term]) {
        self.terms = terms.sorted(by: Self.order)
    }

    /// 文書ごとの出現から一覧を作る（同じ表記・読みはまとめる）
    public init(occurrences: [(document: String, terms: [DocumentTermOccurrence])]) {
        var merged: [DocumentTermOccurrence: Term] = [:]
        for (document, terms) in occurrences {
            for occurrence in terms {
                if var term = merged[occurrence] {
                    term.count += 1
                    if term.documents.last != document && !term.documents.contains(document) {
                        term.documents.append(document)
                    }
                    merged[occurrence] = term
                } else {
                    merged[occurrence] = Term(
                        surface: occurrence.surface, reading: occurrence.reading, count: 1, documents: [document])
                }
            }
        }
        self.init(terms: Array(merged.values))
    }

    /// 指定の文書を除いた一覧（評価で、試す文の文書から集めた語を使わないようにする）
    public func excluding(document: String, occurrences: [DocumentTermOccurrence]) -> DocumentVocabulary {
        var removed: [DocumentTermOccurrence: Int] = [:]
        for occurrence in occurrences { removed[occurrence, default: 0] += 1 }
        var result: [Term] = []
        for var term in terms {
            let key = DocumentTermOccurrence(surface: term.surface, reading: term.reading)
            term.count -= removed[key] ?? 0
            term.documents.removeAll { $0 == document }
            if term.count > 0 { result.append(term) }
        }
        return DocumentVocabulary(terms: result)
    }

    private static func order(_ a: Term, _ b: Term) -> Bool {
        if a.count != b.count { return a.count > b.count }
        if a.surface.count != b.surface.count { return a.surface.count > b.surface.count }
        if a.surface != b.surface { return a.surface < b.surface }
        return a.reading < b.reading
    }
}
