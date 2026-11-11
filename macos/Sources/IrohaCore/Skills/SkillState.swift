import Foundation

/// スキルの解除の状態（`<データフォルダ>/skills.json` の中身。2026-11-11）。
///
/// 算数は次のとおり（設定画面の「スキル」のタブの説明と同じ）:
/// - コード 1 つで、好きなスキルを 1 つ解除できる
/// - 数えるのは別々の作品のコードだけ（同じ作品のコードは 1 つ）
/// - コードが `requiredForAll`（7）つ揃うと、すべてのスキルが解除される
///
/// スキルは名前（文字列）で持つ。どんなスキルがあるかはアプリの側（`Skill`）が決める
public struct SkillState: Codable, Equatable, Sendable {

    /// すべてのスキルが解除されるコードの数
    public static let requiredForAll = 7

    public struct Entry: Codable, Equatable, Sendable {
        /// 作品の番号（コードの先頭 2 文字）
        public var work: String
        /// コード（照合用の形。8 文字）
        public var code: String
        /// このコードを照合した ID。同期の前に別の Mac が別の ID を作っていた場合に備えて、コードごとに持つ
        public var id: String
        /// コードを入れた日時
        public var date: Date
        /// このコードで解除したスキル。まだ選んでいなければ nil
        public var skill: String?
        /// スキルを選んだ日時
        public var chosenAt: Date?

        public init(work: String, code: String, id: String, date: Date, skill: String? = nil, chosenAt: Date? = nil) {
            self.work = work
            self.code = code
            self.id = id
            self.date = date
            self.skill = skill
            self.chosenAt = chosenAt
        }

        var isValid: Bool { SkillCode.code(id: id, work: work) == code }
    }

    public var version = 1
    /// iroha の ID（青空写経に入れてもらう）。まだ作っていなければ nil
    public var id: String?
    public var idCreatedAt: Date?
    public var entries: [Entry]

    public init(id: String? = nil, idCreatedAt: Date? = nil, entries: [Entry] = []) {
        self.id = id
        self.idCreatedAt = idCreatedAt
        self.entries = entries
    }

    public static let empty = SkillState()

    // MARK: - 読み取り

    /// 集めたコードの数
    public var codeCount: Int { entries.count }

    public var isAllUnlocked: Bool { codeCount >= Self.requiredForAll }

    /// まだスキルを選んでいないコードの数（すべて解除されたあとは 0）
    public var unusedCodeCount: Int {
        isAllUnlocked ? 0 : entries.filter { $0.skill == nil }.count
    }

    /// コードで選んだスキル
    public var chosenSkills: Set<String> { Set(entries.compactMap(\.skill)) }

    public func isUnlocked(_ skill: String) -> Bool {
        isAllUnlocked || entries.contains { $0.skill == skill }
    }

    // MARK: - 変更

    public enum AddResult: Equatable, Sendable {
        case added(work: String)
        /// 形が違うか、この ID のコードではない
        case invalid
        /// この作品のコードはもう入れてある
        case alreadyUsed(work: String)
        /// ID をまだ作っていない
        case noID
    }

    public mutating func add(code text: String, date: Date = Date()) -> AddResult {
        guard let id else { return .noID }
        guard let work = SkillCode.work(ofCode: text, id: id),
              let code = SkillCode.code(id: id, work: work) else { return .invalid }
        guard !entries.contains(where: { $0.work == work }) else { return .alreadyUsed(work: work) }
        entries.append(Entry(work: work, code: code, id: id, date: date))
        return .added(work: work)
    }

    /// まだ使っていない最も古いコードで、そのスキルを解除する。使っていないコードがない・もう解除してあれば false
    public mutating func choose(skill: String, date: Date = Date()) -> Bool {
        guard !isUnlocked(skill),
              let index = entries.firstIndex(where: { $0.skill == nil }) else { return false }
        entries[index].skill = skill
        entries[index].chosenAt = date
        return true
    }

    // MARK: - 合わせる

    /// 2 つの状態を合わせる（別の Mac が書いたファイルと、この Mac が覚えている状態）。
    /// どちらかにあるコードは残す。同じ作品のコードが両方にあれば、先に入れたほうを残し、選んだスキルは
    /// 先に選んだほうにする。ID は先に作ったほう
    public func merged(with other: SkillState) -> SkillState {
        var result = SkillState()
        switch (idCreatedAt, other.idCreatedAt) {
        case let (mine?, theirs?):
            (result.id, result.idCreatedAt) = mine <= theirs ? (id, mine) : (other.id, theirs)
        default:
            let source = id != nil ? self : other
            (result.id, result.idCreatedAt) = (source.id, source.idCreatedAt)
        }
        var byWork: [String: Entry] = [:]
        for entry in entries + other.entries {
            guard let existing = byWork[entry.work] else {
                byWork[entry.work] = entry
                continue
            }
            var kept = existing.date <= entry.date ? existing : entry
            let choices = [existing, entry]
                .filter { $0.skill != nil }
                .sorted { ($0.chosenAt ?? .distantFuture) < ($1.chosenAt ?? .distantFuture) }
            kept.skill = choices.first?.skill
            kept.chosenAt = choices.first?.chosenAt
            byWork[entry.work] = kept
        }
        result.entries = byWork.values.sorted { ($0.date, $0.work) < ($1.date, $1.work) }
        return result.normalized()
    }

    /// 照合の合わないコードを捨て、同じスキルを 2 つのコードで選んでいたら後のほうを「選んでいない」に戻す
    /// （2 台の Mac で同期の前に同じスキルを選んだ場合。コードを無駄にしない）
    public func normalized() -> SkillState {
        var result = self
        var seenWorks: Set<String> = []
        result.entries = entries.filter { $0.isValid && seenWorks.insert($0.work).inserted }
        var seenSkills: Set<String> = []
        let order = result.entries.indices
            .filter { result.entries[$0].skill != nil }
            .sorted { (result.entries[$0].chosenAt ?? .distantFuture) < (result.entries[$1].chosenAt ?? .distantFuture) }
        for index in order {
            guard let skill = result.entries[index].skill else { continue }
            if !seenSkills.insert(skill).inserted {
                result.entries[index].skill = nil
                result.entries[index].chosenAt = nil
            }
        }
        return result
    }
}
