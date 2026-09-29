import Foundation

/// ユーザが変換を修正したときに記録する1エントリ
/// （入力の読み全体 → 確定文字列。直す前にエンジンが出していた結果を条件に持つ）。
public struct LearningEntry: Codable, Sendable, Hashable {

    /// ひらがなの読み（変換した入力の全体）
    public var reading: String
    /// 直す前にエンジン（学習を除く）が出していた変換結果。
    /// 次にエンジンがこれと同じ結果を出したときだけ `result` に差し替える。
    /// nil は記録なし（この条件を覚えるようになる前の学習）で、読みが一致すれば常に差し替える
    public var replaced: String?
    /// 確定された変換結果
    public var result: String
    public var updatedAt: Date

    public init(reading: String, replaced: String? = nil, result: String, updatedAt: Date = Date()) {
        self.reading = reading
        self.replaced = replaced
        self.result = result
        self.updatedAt = updatedAt
    }

    /// 同じ読み・同じ「直す前」のエントリは1件にまとめる（新しい方を採る）ためのキー
    struct Key: Hashable {
        var reading: String
        var replaced: String?
    }

    var key: Key { Key(reading: reading, replaced: replaced) }

    // 文節単位の学習（`kind` / `leftContext`）は廃止した。読み込み時は無視し
    // （文節のエントリは `LearningStore.load` が捨てる）、保存時は旧バージョンのirohaが
    // 同じデータフォルダを読めるように従来のキーも書いておく
    private enum CodingKeys: String, CodingKey {
        case kind, reading, replaced, result, leftContext, updatedAt
    }

    public init(from decoder: any Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        reading = try container.decode(String.self, forKey: .reading)
        replaced = try container.decodeIfPresent(String.self, forKey: .replaced)
        result = try container.decode(String.self, forKey: .result)
        updatedAt = try container.decode(Date.self, forKey: .updatedAt)
    }

    public func encode(to encoder: any Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        try container.encode("sentence", forKey: .kind)
        try container.encode(reading, forKey: .reading)
        try container.encodeIfPresent(replaced, forKey: .replaced)
        try container.encode(result, forKey: .result)
        try container.encode("", forKey: .leftContext)
        try container.encode(updatedAt, forKey: .updatedAt)
    }
}

/// 変換時に参照する学習結果の不変スナップショット。
///
/// 覚えるのは「入力の読み全体 → 確定した文字列」と、そのとき直す前にエンジンが出していた結果。
/// 次に同じ読みを丸ごと入力し、エンジンが直す前と同じ結果を出したときにだけ差し替える。
/// 左文脈が変わってエンジンの結果が変われば（「御恩と」の後の「ほうこう」→「奉公」など）、
/// 差し替えずにエンジンの結果を使う。文脈そのものは覚えない（文節単位の学習は、同じ語が
/// 設定画面で2行に見えるうえ、適用の条件（直前の文脈）がユーザから見て分からないので廃止した。
/// 「直す前」は設定画面の一覧に列として出る）
public struct LearningDictionary: Sendable {

    public static let empty = LearningDictionary(entries: [])

    public let entries: [LearningEntry]
    /// 読み全体 → その読みのエントリ（新しい順。同じ「直す前」は1件）
    private let byReading: [String: [LearningEntry]]

    public init(entries: [LearningEntry]) {
        self.entries = entries
        var byReading: [String: [LearningEntry]] = [:]
        var seen: Set<LearningEntry.Key> = []
        for entry in entries.sorted(by: { $0.updatedAt > $1.updatedAt }) {
            guard !entry.reading.isEmpty, !entry.result.isEmpty else { continue }
            // 同じキーなら新しい方を採る（並べ替え済みなので先勝ち）
            guard seen.insert(entry.key).inserted else { continue }
            byReading[entry.reading, default: []].append(entry)
        }
        self.byReading = byReading
    }

    public var isEmpty: Bool { byReading.isEmpty }

    /// 入力全体が過去に確定した読みと完全一致するエントリ（新しい順）
    public func entries(forReading reading: String) -> [LearningEntry] {
        byReading[reading] ?? []
    }

    /// エンジンが `engineResult` を出したときに差し替えるエントリ。
    ///
    /// 「直す前」が `engineResult` と一致するものを採り、無ければ「直す前」の記録がないものを採る
    public func entry(forReading reading: String, engineResult: String?) -> LearningEntry? {
        let candidates = entries(forReading: reading)
        if let engineResult, let exact = candidates.first(where: { $0.replaced == engineResult }) {
            return exact
        }
        return candidates.first { $0.replaced == nil }
    }
}
