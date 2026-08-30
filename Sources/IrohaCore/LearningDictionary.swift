import Foundation

/// ユーザが変換を修正したときに記録する1エントリ。
public struct LearningEntry: Codable, Sendable, Hashable {

    public enum Kind: String, Codable, Sendable {
        case sentence   // 入力全体（読み全体 → 確定文字列）
        case segment    // 文節（読み → 変換結果、直前の文脈つき）
    }

    public var kind: Kind
    /// ひらがなの読み
    public var reading: String
    /// 確定された変換結果
    public var result: String
    /// この入力内で直前までに確定していた文字列の末尾（最大`LearningDictionary.contextLength`文字）。
    /// 入力の先頭の文節なら空。`kind == .sentence`では使わない
    public var leftContext: String
    public var updatedAt: Date

    public init(
        kind: Kind, reading: String, result: String, leftContext: String = "",
        updatedAt: Date = Date()
    ) {
        self.kind = kind
        self.reading = reading
        self.result = result
        self.leftContext = leftContext
        self.updatedAt = updatedAt
    }
}

/// 変換時に参照する学習結果の不変スナップショット。
///
/// 同じ読みでも文中の位置によって正解が違う（「きしゃのきしゃ」→「記者の貴社」）ため、
/// 文節の学習は「直前までに確定した文字列」を条件に付けて記録・適用する。
public struct LearningDictionary: Sendable {

    /// 文脈として覚える文字数
    public static let contextLength = 6
    /// 文中の一部として当てはめる最短の読みの長さ（1文字だと無差別に一致して変換が壊れる）
    public static let minimumMatchLength = 2

    public static let empty = LearningDictionary(entries: [])

    public let entries: [LearningEntry]
    /// 読み全体 → 確定文字列
    private let sentences: [String: LearningEntry]
    /// 読み → 文節エントリ（新しい順）
    private let segments: [String: [LearningEntry]]
    private let maxSegmentReadingLength: Int

    public init(entries: [LearningEntry]) {
        self.entries = entries
        var sentences: [String: LearningEntry] = [:]
        var segments: [String: [LearningEntry]] = [:]
        var maxLength = 0
        for entry in entries.sorted(by: { $0.updatedAt > $1.updatedAt }) {
            guard !entry.reading.isEmpty, !entry.result.isEmpty else { continue }
            switch entry.kind {
            case .sentence:
                // 同じ読みなら新しい方を採る（並べ替え済みなので先勝ち）
                if sentences[entry.reading] == nil { sentences[entry.reading] = entry }
            case .segment:
                segments[entry.reading, default: []].append(entry)
                maxLength = max(maxLength, entry.reading.count)
            }
        }
        self.sentences = sentences
        self.segments = segments
        self.maxSegmentReadingLength = maxLength
    }

    public var isEmpty: Bool { sentences.isEmpty && segments.isEmpty }

    // MARK: - 参照

    /// 入力全体が過去に確定した読みと完全一致するならその確定文字列
    public func sentence(forReading reading: String) -> String? {
        sentences[reading]?.result
    }

    /// 位置`index`から始まる学習済み文節が適用され得るか（文脈条件込みの安価な事前判定）。
    ///
    /// 直前の未変換部分を先にエンジンへ渡して文脈を確定させる必要があるので、
    /// 「変換してみるまで分からない」条件（空でない`leftContext`）はここでは真として扱う
    public func mayMatch(_ characters: [Character], from index: Int, atStart: Bool) -> Bool {
        forEachCandidateReading(characters, from: index) { reading in
            guard let candidates = segments[reading] else { return false }
            // 文脈なしで覚えたものは入力の先頭でしか使わない
            return candidates.contains { !$0.leftContext.isEmpty || atStart }
        }
    }

    /// 位置`index`から始まる学習済み文節のうち、文脈条件を満たす最長のもの
    public func bestMatch(
        _ characters: [Character], from index: Int, leftContext: String, atStart: Bool
    ) -> LearningEntry? {
        var found: LearningEntry?
        _ = forEachCandidateReading(characters, from: index) { reading in
            guard let candidates = segments[reading] else { return false }
            // 新しい順に見て、最初に文脈が合ったものを採る
            guard let match = candidates.first(where: {
                applies($0, leftContext: leftContext, atStart: atStart)
            }) else { return false }
            found = match
            return true
        }
        return found
    }

    /// 文脈条件の判定
    public func applies(_ entry: LearningEntry, leftContext: String, atStart: Bool) -> Bool {
        entry.leftContext.isEmpty ? atStart : leftContext.hasSuffix(entry.leftContext)
    }

    /// 候補ウィンドウ用: この読みで学習済みの変換結果を、文脈が合うものから新しい順に返す
    public func learnedResults(forReading reading: String, leftContext: String) -> [String] {
        var applicable: [String] = []
        var others: [String] = []
        for entry in segments[reading] ?? [] {
            // 候補ウィンドウは明示的な操作なので、文脈が合わないものも後ろに残す
            if applies(entry, leftContext: leftContext, atStart: leftContext.isEmpty) {
                applicable.append(entry.result)
            } else {
                others.append(entry.result)
            }
        }
        if let sentence = sentences[reading]?.result {
            applicable.insert(sentence, at: 0)
        }
        var results: [String] = []
        for result in applicable + others where !results.contains(result) {
            results.append(result)
        }
        return results
    }

    // MARK: - 内部

    /// 位置`index`から始まる読みを長い方から順に`body`へ渡す。`body`が真を返したら打ち切る
    private func forEachCandidateReading(
        _ characters: [Character], from index: Int, _ body: (String) -> Bool
    ) -> Bool {
        var length = min(maxSegmentReadingLength, characters.count - index)
        while length >= Self.minimumMatchLength {
            if body(String(characters[index..<(index + length)])) { return true }
            length -= 1
        }
        return false
    }
}
