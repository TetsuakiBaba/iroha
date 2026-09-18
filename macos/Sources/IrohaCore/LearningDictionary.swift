import Foundation

/// ユーザが変換を修正したときに記録する1エントリ（入力の読み全体 → 確定文字列）。
public struct LearningEntry: Codable, Sendable, Hashable {

    /// ひらがなの読み（変換した入力の全体）
    public var reading: String
    /// 確定された変換結果
    public var result: String
    public var updatedAt: Date

    public init(reading: String, result: String, updatedAt: Date = Date()) {
        self.reading = reading
        self.result = result
        self.updatedAt = updatedAt
    }

    // 文節単位の学習（`kind` / `leftContext`）は廃止した。読み込み時は無視し
    // （文節のエントリは `LearningStore.load` が捨てる）、保存時は旧バージョンのirohaが
    // 同じデータフォルダを読めるように従来のキーも書いておく
    private enum CodingKeys: String, CodingKey {
        case kind, reading, result, leftContext, updatedAt
    }

    public init(from decoder: any Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        reading = try container.decode(String.self, forKey: .reading)
        result = try container.decode(String.self, forKey: .result)
        updatedAt = try container.decode(Date.self, forKey: .updatedAt)
    }

    public func encode(to encoder: any Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        try container.encode("sentence", forKey: .kind)
        try container.encode(reading, forKey: .reading)
        try container.encode(result, forKey: .result)
        try container.encode("", forKey: .leftContext)
        try container.encode(updatedAt, forKey: .updatedAt)
    }
}

/// 変換時に参照する学習結果の不変スナップショット。
///
/// 覚えるのは「入力の読み全体 → 確定した文字列」だけで、次に同じ読みを丸ごと入力したときに
/// その結果を返す。文中の一部に当てはめることはしない（文節単位の学習は、同じ語が設定画面で
/// 2行に見えるうえ、適用の条件（直前の文脈）がユーザから見て分からないので廃止した）
public struct LearningDictionary: Sendable {

    public static let empty = LearningDictionary(entries: [])

    public let entries: [LearningEntry]
    /// 読み全体 → エントリ
    private let byReading: [String: LearningEntry]

    public init(entries: [LearningEntry]) {
        self.entries = entries
        var byReading: [String: LearningEntry] = [:]
        for entry in entries.sorted(by: { $0.updatedAt > $1.updatedAt }) {
            guard !entry.reading.isEmpty, !entry.result.isEmpty else { continue }
            // 同じ読みなら新しい方を採る（並べ替え済みなので先勝ち）
            if byReading[entry.reading] == nil { byReading[entry.reading] = entry }
        }
        self.byReading = byReading
    }

    public var isEmpty: Bool { byReading.isEmpty }

    /// 入力全体が過去に確定した読みと完全一致するならその確定文字列
    public func result(forReading reading: String) -> String? {
        byReading[reading]?.result
    }
}
