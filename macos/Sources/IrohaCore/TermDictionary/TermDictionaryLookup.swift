import Foundation

/// 候補が、入れている専門辞書のどれに載っている語かを引く（候補ウィンドウで辞書名の印を出すため）。
///
/// 辞書ファイル（`<データフォルダ>/dictionaries/<ID>.tsv`）は「読み<TAB>語」の行が読み・語の順（UTF-8 のバイト順。
/// `build-term-dictionaries.py` が Python の `sorted` で並べる）に並んでいるので、ファイルをメモリに
/// マップして二分探索で引く。語の一覧をメモリに展開しない（人名だけで 21 万語ある）
public struct TermDictionaryLookup: Sendable {

    struct Table: Sendable {
        /// 印に出す名前（「工学」「地名」）
        let label: String
        let data: Data
        /// 先頭の「#」行を除いた、最初の語の行の位置
        let start: Int
    }

    let tables: [Table]

    public static let empty = TermDictionaryLookup(tables: [])

    init(tables: [Table]) {
        self.tables = tables
    }

    /// 入れている辞書のファイルをマップする（読めないファイルは除く）
    public init(records: [TermDictionaryInstall.Record]) {
        tables = records.compactMap { record in
            guard let data = try? Data(contentsOf: TermDictionaryInstall.fileURL(id: record.id), options: .alwaysMapped)
            else { return nil }
            return Table(label: Self.label(for: record.name), data: data, start: Self.bodyStart(data))
        }
    }

    /// 辞書ファイルを直接マップする（iroha-cli の `IROHA_TERM_LIVE` 用。印の名前はファイル名）
    public init(files: [URL]) {
        tables = files.compactMap { url in
            guard let data = try? Data(contentsOf: url, options: .alwaysMapped) else { return nil }
            return Table(label: url.deletingPathExtension().lastPathComponent, data: data, start: Self.bodyStart(data))
        }
    }

    public var isEmpty: Bool { tables.isEmpty }

    /// マップした辞書の数
    public var dictionaryCount: Int { tables.count }

    /// 印に出す名前。カタログの名前の括弧書きは除く（「地名（市区町村・町域）」→「地名」）
    static func label(for name: String) -> String {
        guard let open = name.firstIndex(where: { $0 == "（" || $0 == "(" }), open != name.startIndex else { return name }
        return String(name[..<open])
    }

    /// 文節の候補 `candidate`（読み `reading`）に含まれる専門辞書の語が載っている辞書の名前。
    ///
    /// 候補が「辞書の語」か「辞書の語＋読みの残り（助詞などのかな）」のときに当てる（「ちらくが」→「地絡が」の「地絡」）。
    /// 読みの長い語から試し、最初に当たった語の辞書をすべて返す。当たらなければ空
    public func labels(reading: String, candidate: String) -> [String] {
        guard !tables.isEmpty else { return [] }
        let readingCharacters = Array(reading)
        let candidateCount = candidate.count
        var length = readingCharacters.count
        while length >= TermDictionary.minimumReadingLength {
            let tail = String(readingCharacters[length...])
            if candidate.hasSuffix(tail), candidateCount > tail.count {
                let word = String(candidate.dropLast(tail.count))
                let termReading = String(readingCharacters[..<length])
                if word != termReading {
                    let key = Array("\(termReading)\t\(word)".utf8)
                    let found = tables.filter { Self.contains(key, in: $0) }.map(\.label)
                    if !found.isEmpty { return found }
                }
            }
            length -= 1
        }
        return []
    }

    // MARK: - ライブ変換に使う語

    /// 読み `reading` の中に読みが現れる語のうち、ライブ変換に使えるものを、ユーザ辞書の形にして返す
    /// （`TermDictionaryLiveEngine` がユーザ辞書と同じ規則で読みに当てる）。
    ///
    /// 使うのは次の語だけ:
    /// - 読みが `TermDictionary.minimumReadingLength` 文字以上
    /// - その読みの語が、ライブ変換に使う辞書全体で 1 つだけ（「いがん」→ 胃がん・胃ガン・胃癌 のように
    ///   表記が分かれる読みは、どれに置き換えるかを決められないので候補ウィンドウにだけ出す）
    /// - 語が読みより大幅に長くない（`UserDictionary.isSuitableForLiveConversion`）
    ///
    /// 読みの位置ごとに、その位置から始まる読みを持つ語が辞書にある間だけ 1 文字ずつ延ばして引く
    /// （語の一覧をメモリに展開しない）
    public func liveDictionary(occurringIn reading: String) -> UserDictionary {
        guard !tables.isEmpty else { return .empty }
        let characters = Array(reading)
        var entries: [UserDictionaryEntry] = []
        var seen: Set<String> = []
        for start in characters.indices {
            var length = 1
            while start + length <= characters.count {
                let part = String(characters[start..<(start + length)])
                let prefix = Array(part.utf8)
                guard tables.contains(where: { Self.hasLine(withPrefix: prefix, in: $0) }) else { break }
                if length >= TermDictionary.minimumReadingLength, seen.insert(part).inserted {
                    let key = prefix + [Self.tab]
                    var words: [String] = []
                    for table in tables {
                        for word in Self.words(withKey: key, in: table) where !words.contains(word) {
                            words.append(word)
                        }
                    }
                    if words.count == 1, words[0] != part,
                       UserDictionary.isSuitableForLiveConversion(reading: part, word: words[0]) {
                        entries.append(UserDictionaryEntry(reading: part, word: words[0]))
                    }
                }
                length += 1
            }
        }
        return entries.isEmpty ? .empty : UserDictionary(entries: entries)
    }

    // MARK: - 二分探索

    private static let tab = UInt8(ascii: "\t")

    private static let newline = UInt8(ascii: "\n")

    /// 「#」で始まる行（出典・ライセンスの注記）の後ろの位置
    static func bodyStart(_ data: Data) -> Int {
        data.withUnsafeBytes { (bytes: UnsafeRawBufferPointer) -> Int in
            var position = 0
            while position < bytes.count, bytes[position] == UInt8(ascii: "#") {
                while position < bytes.count, bytes[position] != newline { position += 1 }
                position += 1
            }
            return min(position, bytes.count)
        }
    }

    /// 鍵より前に並ぶ行を除いた、最初の行の頭の位置（無ければファイルの終わり）
    static func lowerBound(_ key: [UInt8], in table: Table) -> Int {
        table.data.withUnsafeBytes { (bytes: UnsafeRawBufferPointer) -> Int in
            var low = table.start
            var high = bytes.count
            while low < high {
                var lineStart = (low + high) / 2
                while lineStart > low, bytes[lineStart - 1] != newline { lineStart -= 1 }
                var lineEnd = lineStart
                while lineEnd < high, bytes[lineEnd] != newline { lineEnd += 1 }
                if compare(bytes, lineStart..<lineEnd, key) < 0 {
                    low = lineEnd + 1
                } else {
                    high = lineStart
                }
            }
            return low
        }
    }

    /// `prefix` で始まる行があるか
    static func hasLine(withPrefix prefix: [UInt8], in table: Table) -> Bool {
        let position = lowerBound(prefix, in: table)
        return table.data.withUnsafeBytes { (bytes: UnsafeRawBufferPointer) -> Bool in
            starts(bytes, at: position, with: prefix)
        }
    }

    /// 「読み<TAB>」（`key`）で始まる行の語
    static func words(withKey key: [UInt8], in table: Table) -> [String] {
        var position = lowerBound(key, in: table)
        return table.data.withUnsafeBytes { (bytes: UnsafeRawBufferPointer) -> [String] in
            var words: [String] = []
            while starts(bytes, at: position, with: key) {
                var lineEnd = position + key.count
                while lineEnd < bytes.count, bytes[lineEnd] != newline { lineEnd += 1 }
                let word = String(decoding: bytes[(position + key.count)..<lineEnd], as: UTF8.self)
                if !word.isEmpty { words.append(word) }
                position = lineEnd + 1
            }
            return words
        }
    }

    private static func starts(_ bytes: UnsafeRawBufferPointer, at position: Int, with prefix: [UInt8]) -> Bool {
        guard position + prefix.count <= bytes.count else { return false }
        for offset in 0..<prefix.count where bytes[position + offset] != prefix[offset] {
            return false
        }
        return true
    }

    static func contains(_ key: [UInt8], in table: Table) -> Bool {
        table.data.withUnsafeBytes { (bytes: UnsafeRawBufferPointer) -> Bool in
            var low = table.start
            var high = bytes.count
            while low < high {
                // 真ん中を含む行の頭と終わり
                var lineStart = (low + high) / 2
                while lineStart > low, bytes[lineStart - 1] != newline { lineStart -= 1 }
                var lineEnd = lineStart
                while lineEnd < high, bytes[lineEnd] != newline { lineEnd += 1 }
                switch compare(bytes, lineStart..<lineEnd, key) {
                case 0: return true
                case ..<0: low = lineEnd + 1
                default: high = lineStart
                }
            }
            return false
        }
    }

    /// 行と鍵をバイト列として比べる（負: 行が前、0: 同じ、正: 行が後ろ）
    private static func compare(_ bytes: UnsafeRawBufferPointer, _ line: Range<Int>, _ key: [UInt8]) -> Int {
        let length = min(line.count, key.count)
        for offset in 0..<length {
            let a = bytes[line.lowerBound + offset]
            let b = key[offset]
            if a != b { return a < b ? -1 : 1 }
        }
        return line.count - key.count
    }
}
