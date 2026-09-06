import Foundation

/// ユーザ定義の変換ルール（User Rewriter）の1件。
///
/// 読み（トリガー）が一致したとき、テンプレート（出力）を展開した文字列を変換候補に加える。
/// ユーザ辞書と違い出力は変換のたびに計算されるので、日付や時刻のように
/// 動的に変わる文字列を扱える（`{{date:yyyy/MM/dd}}` など。書式は `RewriteTemplate` を参照）。
///
/// 今はトリガーの完全一致（`.exact`）だけだが、`TriggerKind` と `RewriteMatch.parameters`
/// を通して、将来パラメータ付きトリガー（「Nにちご」→ N日後の日付 等）を追加できる構造にしてある。
public struct UserRewriteRule: Codable, Sendable, Hashable, Identifiable {

    /// トリガーの一致方法。値を追加するときは `UserRewriteRuleSet.matches(forReading:)` に
    /// その一致処理を足し、一致で得た値は `RewriteMatch.parameters` で出力テンプレートへ渡す
    public enum TriggerKind: String, Codable, Sendable {
        case exact   // 読み全体がトリガーと完全一致
    }

    public var id: UUID
    public var kind: TriggerKind
    /// トリガー（読み）。ひらがなに正規化して保持する
    public var trigger: String
    /// 出力テンプレート（`RewriteTemplate` の書式）
    public var output: String
    public var isEnabled: Bool

    public init(
        id: UUID = UUID(), kind: TriggerKind = .exact, trigger: String, output: String,
        isEnabled: Bool = true
    ) {
        self.id = id
        self.kind = kind
        self.trigger = UserRewriteRule.normalizedTrigger(trigger)
        self.output = output
        self.isEnabled = isEnabled
    }

    /// トリガーの正規化はユーザ辞書の読みと同じ（カタカナ→ひらがな、前後の空白除去）
    public static func normalizedTrigger(_ text: String) -> String {
        UserDictionary.normalizedReading(text)
    }

    // 旧いファイルに `kind` / `isEnabled` が無くても読めるようにする
    private enum CodingKeys: String, CodingKey { case id, kind, trigger, output, isEnabled }

    public init(from decoder: any Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decodeIfPresent(UUID.self, forKey: .id) ?? UUID()
        kind = try container.decodeIfPresent(TriggerKind.self, forKey: .kind) ?? .exact
        trigger = Self.normalizedTrigger(try container.decode(String.self, forKey: .trigger))
        output = try container.decode(String.self, forKey: .output)
        isEnabled = try container.decodeIfPresent(Bool.self, forKey: .isEnabled) ?? true
    }
}

/// テンプレート展開時の環境。日時のほか、将来のパラメータ付きトリガーが
/// 取り出した値（`parameters`）をテンプレートへ渡す口を持つ
public struct RewriteContext: Sendable {
    public var now: Date
    public var calendar: Calendar
    public var locale: Locale
    public var timeZone: TimeZone
    /// トリガーの一致で得た名前付きの値（`{{param:名前}}` で参照）。完全一致では空
    public var parameters: [String: String]

    public init(
        now: Date = Date(),
        calendar: Calendar = Calendar(identifier: .gregorian),
        locale: Locale = Locale(identifier: "ja_JP"),
        timeZone: TimeZone = .current,
        parameters: [String: String] = [:]
    ) {
        self.now = now
        self.calendar = calendar
        self.locale = locale
        self.timeZone = timeZone
        self.parameters = parameters
    }
}

/// 出力テンプレート。通常の文字列と `{{名前}}` / `{{名前:引数}}` のプレースホルダを混在できる。
///
/// 対応するプレースホルダ（引数は省略可。日時の書式はUnicode/ICUのパターン）:
/// - `{{date:yyyy/MM/dd}}`     今日の日付（既定 `yyyy/MM/dd`）
/// - `{{time:HH:mm}}`          現在時刻（既定 `HH:mm`）
/// - `{{datetime:yyyy/MM/dd HH:mm}}` 日付と時刻（既定 `yyyy/MM/dd HH:mm`）
/// - `{{wareki:Gy年M月d日}}`   和暦の日付（既定 `Gy年M月d日` → 令和8年9月6日）
/// - `{{param:名前}}`          トリガーの一致で得た値（将来のパラメータ付きトリガー用）
///
/// 引数の中の `:`（`HH:mm` など）はそのまま引数の一部になる（名前と引数は最初の `:` で分ける）。
/// 知らないプレースホルダや閉じていない `{{` は書いたまま文字列として残す
/// （設定画面ではプレビューでそのまま見えるので、書き損じに気づける）。
public struct RewriteTemplate: Sendable, Equatable {

    public enum Segment: Sendable, Equatable {
        case text(String)
        case placeholder(name: String, argument: String?)
    }

    public static let knownPlaceholders: Set<String> = ["date", "time", "datetime", "wareki", "param"]

    public let source: String
    public let segments: [Segment]

    public init(_ source: String) {
        self.source = source
        self.segments = Self.parse(source)
    }

    /// 展開結果が呼ぶたびに変わりうるか（日時などを含む）
    public var isDynamic: Bool {
        segments.contains {
            if case .placeholder(let name, _) = $0 { return Self.knownPlaceholders.contains(name) }
            return false
        }
    }

    /// 対応していないプレースホルダ名（設定画面の警告用）
    public var unknownPlaceholders: [String] {
        segments.compactMap {
            if case .placeholder(let name, _) = $0, !Self.knownPlaceholders.contains(name) { return name }
            return nil
        }
    }

    public func render(in context: RewriteContext = RewriteContext()) -> String {
        var result = ""
        for segment in segments {
            switch segment {
            case .text(let text):
                result += text
            case .placeholder(let name, let argument):
                result += Self.expand(name: name, argument: argument, context: context)
                    ?? Self.literal(name: name, argument: argument)
            }
        }
        return result
    }

    // MARK: - 展開

    private static func expand(name: String, argument: String?, context: RewriteContext) -> String? {
        switch name {
        case "date":
            return formatDate(argument ?? "yyyy/MM/dd", context: context)
        case "time":
            return formatDate(argument ?? "HH:mm", context: context)
        case "datetime":
            return formatDate(argument ?? "yyyy/MM/dd HH:mm", context: context)
        case "wareki":
            var context = context
            context.calendar = Calendar(identifier: .japanese)
            return formatDate(argument ?? "Gy年M月d日", context: context)
        case "param":
            guard let argument else { return nil }
            return context.parameters[argument]
        default:
            return nil
        }
    }

    private static func formatDate(_ format: String, context: RewriteContext) -> String {
        let formatter = DateFormatter()
        formatter.calendar = context.calendar
        formatter.locale = context.locale
        formatter.timeZone = context.timeZone
        formatter.dateFormat = format
        return formatter.string(from: context.now)
    }

    private static func literal(name: String, argument: String?) -> String {
        if let argument { return "{{\(name):\(argument)}}" }
        return "{{\(name)}}"
    }

    // MARK: - 解析

    private static func parse(_ source: String) -> [Segment] {
        var segments: [Segment] = []
        var text = ""
        var rest = Substring(source)

        while let open = rest.range(of: "{{") {
            let afterOpen = rest[open.upperBound...]
            guard let close = afterOpen.range(of: "}}") else { break }
            let body = afterOpen[..<close.lowerBound]
            // 名前と引数は最初の「:」で分ける（引数側の「:」は引数の一部）
            let name: String
            let argument: String?
            if let colon = body.firstIndex(of: ":") {
                name = body[..<colon].trimmingCharacters(in: .whitespaces)
                argument = String(body[body.index(after: colon)...])
            } else {
                name = body.trimmingCharacters(in: .whitespaces)
                argument = nil
            }
            text += rest[..<open.lowerBound]
            if name.isEmpty {
                // 「{{}}」のような空のプレースホルダはそのまま文字として扱う
                text += rest[open.lowerBound..<close.upperBound]
            } else {
                if !text.isEmpty {
                    segments.append(.text(text))
                    text = ""
                }
                segments.append(.placeholder(name: name, argument: argument))
            }
            rest = afterOpen[close.upperBound...]
        }
        text += rest
        if !text.isEmpty { segments.append(.text(text)) }
        return segments
    }
}

/// トリガーの一致結果。パラメータ付きトリガーを導入したときは、取り出した値を
/// `parameters` に入れて `RewriteContext` 経由でテンプレートへ渡す
public struct RewriteMatch: Sendable, Equatable {
    public let rule: UserRewriteRule
    public let parameters: [String: String]

    public init(rule: UserRewriteRule, parameters: [String: String] = [:]) {
        self.rule = rule
        self.parameters = parameters
    }
}

/// 候補生成時に参照する変換ルールの不変スナップショット（有効なルールの索引つき）。
///
/// かな漢字変換エンジン（学習・ユーザ辞書・LLM）とは独立した候補生成源で、
/// 文節の候補ウィンドウを開くときにコントローラが `candidates(forReading:)` を呼び、
/// エンジンの候補に合流させる。ライブ変換や第一候補には影響しない
public struct UserRewriteRuleSet: Sendable {

    public static let empty = UserRewriteRuleSet(rules: [])

    /// 全ルール（無効なものも含む。設定画面の一覧用、登録順）
    public let rules: [UserRewriteRule]
    /// 完全一致トリガー → 有効なルール（登録順）
    private let exactRules: [String: [UserRewriteRule]]

    public init(rules: [UserRewriteRule]) {
        self.rules = rules
        var exact: [String: [UserRewriteRule]] = [:]
        for rule in rules where rule.isEnabled && !rule.trigger.isEmpty && !rule.output.isEmpty {
            switch rule.kind {
            case .exact:
                exact[rule.trigger, default: []].append(rule)
            }
        }
        self.exactRules = exact
    }

    /// 有効なルールが1つもないか
    public var isEmpty: Bool { exactRules.isEmpty }

    /// 読みに一致するルール（登録順）
    public func matches(forReading reading: String) -> [RewriteMatch] {
        guard !isEmpty else { return [] }
        let normalized = UserRewriteRule.normalizedTrigger(reading)
        // 一致方法を追加するときはここに分岐を足す（`.exact` 以外は索引を別に持つ）
        return (exactRules[normalized] ?? []).map { RewriteMatch(rule: $0) }
    }

    /// 読みに一致するルールの出力を展開した候補（登録順、重複と空は除く）
    public func candidates(forReading reading: String, context: RewriteContext = RewriteContext()) -> [String] {
        var results: [String] = []
        for match in matches(forReading: reading) {
            var context = context
            context.parameters = match.parameters
            let rendered = RewriteTemplate(match.rule.output).render(in: context)
            guard !rendered.isEmpty, !results.contains(rendered) else { continue }
            results.append(rendered)
        }
        return results
    }
}
