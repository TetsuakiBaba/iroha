import XCTest
@testable import IrohaCore

final class UserRewriteRuleTests: XCTestCase {

    /// 2026-09-06 14:05:09（日本時間）に固定した展開環境
    private func fixedContext(parameters: [String: String] = [:]) -> RewriteContext {
        var components = DateComponents()
        components.year = 2026
        components.month = 9
        components.day = 6
        components.hour = 14
        components.minute = 5
        components.second = 9
        let timeZone = TimeZone(identifier: "Asia/Tokyo")!
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        let now = calendar.date(from: components)!
        return RewriteContext(now: now, timeZone: timeZone, parameters: parameters)
    }

    // MARK: - テンプレートの解析

    func testParsePlainTextIsSingleSegment() {
        XCTAssertEqual(RewriteTemplate("こんにちは").segments, [.text("こんにちは")])
        XCTAssertFalse(RewriteTemplate("こんにちは").isDynamic)
    }

    func testParseSplitsNameAndArgumentAtFirstColon() {
        let template = RewriteTemplate("いま {{time:HH:mm:ss}} です")
        XCTAssertEqual(template.segments, [
            .text("いま "),
            .placeholder(name: "time", argument: "HH:mm:ss"),
            .text(" です"),
        ])
        XCTAssertTrue(template.isDynamic)
    }

    func testParsePlaceholderWithoutArgument() {
        XCTAssertEqual(RewriteTemplate("{{date}}").segments, [.placeholder(name: "date", argument: nil)])
        XCTAssertEqual(RewriteTemplate("{{ date }}").segments, [.placeholder(name: "date", argument: nil)])
    }

    func testParseLeavesUnclosedAndEmptyBracesAsText() {
        XCTAssertEqual(RewriteTemplate("{{date").segments, [.text("{{date")])
        XCTAssertEqual(RewriteTemplate("a{{}}b").segments, [.text("a{{}}b")])
    }

    // MARK: - テンプレートの展開

    func testRenderDateAndTimeWithDefaultsAndFormats() {
        let context = fixedContext()
        XCTAssertEqual(RewriteTemplate("{{date}}").render(in: context), "2026/09/06")
        XCTAssertEqual(RewriteTemplate("{{time}}").render(in: context), "14:05")
        XCTAssertEqual(RewriteTemplate("{{datetime}}").render(in: context), "2026/09/06 14:05")
        XCTAssertEqual(RewriteTemplate("{{date:yyyy-MM-dd}}").render(in: context), "2026-09-06")
        XCTAssertEqual(RewriteTemplate("{{time:HH:mm:ss}}").render(in: context), "14:05:09")
        XCTAssertEqual(RewriteTemplate("{{date:M月d日(E)}}").render(in: context), "9月6日(日)")
    }

    func testRenderWareki() {
        XCTAssertEqual(RewriteTemplate("{{wareki}}").render(in: fixedContext()), "令和8年9月6日")
    }

    func testRenderDayOffsets() {
        let context = fixedContext()
        XCTAssertEqual(RewriteTemplate("{{date+1}}").render(in: context), "2026/09/07")
        XCTAssertEqual(RewriteTemplate("{{date-2:M月d日}}").render(in: context), "9月4日")
        XCTAssertEqual(RewriteTemplate("{{date+30}}").render(in: context), "2026/10/06")   // 月をまたぐ
        XCTAssertEqual(RewriteTemplate("{{date+120}}").render(in: context), "2027/01/04")  // 年をまたぐ
        XCTAssertEqual(RewriteTemplate("{{wareki+3}}").render(in: context), "令和8年9月9日")
        XCTAssertEqual(RewriteTemplate("{{datetime-1}}").render(in: context), "2026/09/05 14:05")
        XCTAssertTrue(RewriteTemplate("{{date+1}}").isDynamic)
    }

    func testOffsetNotAllowedOnTimeOrMalformed() {
        // time にはオフセットを付けられず、数字でないオフセットも未対応として書いたまま残す
        XCTAssertEqual(RewriteTemplate("{{time+1}}").render(in: fixedContext()), "{{time+1}}")
        XCTAssertEqual(RewriteTemplate("{{time+1}}").unknownPlaceholders, ["time+1"])
        XCTAssertEqual(RewriteTemplate("{{date+}}").unknownPlaceholders, ["date+"])
        XCTAssertEqual(RewriteTemplate("{{date+x}}").unknownPlaceholders, ["date+x"])
        XCTAssertEqual(RewriteTemplate("{{date+1-1}}").unknownPlaceholders, ["date+1-1"])
    }

    func testRenderMixesTextAndPlaceholders() {
        let template = RewriteTemplate("本日（{{date:yyyy/MM/dd}}）{{time:HH:mm}}現在")
        XCTAssertEqual(template.render(in: fixedContext()), "本日（2026/09/06）14:05現在")
    }

    func testRenderKeepsUnknownPlaceholderLiteral() {
        let template = RewriteTemplate("{{unknown:x}}と{{nope}}")
        XCTAssertEqual(template.render(in: fixedContext()), "{{unknown:x}}と{{nope}}")
        XCTAssertEqual(template.unknownPlaceholders, ["unknown", "nope"])
        XCTAssertFalse(template.isDynamic)
    }

    func testRenderParameterFromContext() {
        let context = fixedContext(parameters: ["n": "3"])
        XCTAssertEqual(RewriteTemplate("{{param:n}}日後").render(in: context), "3日後")
        // 無いパラメータは書いたまま残す
        XCTAssertEqual(RewriteTemplate("{{param:m}}").render(in: context), "{{param:m}}")
    }

    // MARK: - ルール集合

    func testCandidatesMatchExactTriggerOnly() {
        let rules = UserRewriteRuleSet(rules: [
            UserRewriteRule(trigger: "きょう", output: "{{date:yyyy/MM/dd}}"),
            UserRewriteRule(trigger: "いま", output: "{{time:HH:mm}}"),
        ])
        let context = fixedContext()
        XCTAssertEqual(rules.candidates(forReading: "きょう", context: context), ["2026/09/06"])
        XCTAssertEqual(rules.candidates(forReading: "いま", context: context), ["14:05"])
        XCTAssertEqual(rules.candidates(forReading: "きょうは", context: context), [])
        XCTAssertEqual(rules.candidates(forReading: "きょ", context: context), [])
    }

    func testCandidatesNormalizeReadingLikeUserDictionary() {
        let rules = UserRewriteRuleSet(rules: [UserRewriteRule(trigger: "キョウ", output: "A")])
        XCTAssertEqual(rules.rules.first?.trigger, "きょう")
        XCTAssertEqual(rules.candidates(forReading: "きょう"), ["A"])
        XCTAssertEqual(rules.candidates(forReading: " キョウ "), ["A"])
    }

    func testCandidatesKeepRegistrationOrderAndDropDuplicates() {
        let rules = UserRewriteRuleSet(rules: [
            UserRewriteRule(trigger: "きょう", output: "{{date:yyyy/MM/dd}}"),
            UserRewriteRule(trigger: "きょう", output: "{{date:M月d日}}"),
            UserRewriteRule(trigger: "きょう", output: "{{date:yyyy/MM/dd}}"),
        ])
        XCTAssertEqual(
            rules.candidates(forReading: "きょう", context: fixedContext()),
            ["2026/09/06", "9月6日"])
    }

    func testDisabledAndEmptyRulesAreIgnored() {
        let rules = UserRewriteRuleSet(rules: [
            UserRewriteRule(trigger: "きょう", output: "X", isEnabled: false),
            UserRewriteRule(trigger: "", output: "Y"),
            UserRewriteRule(trigger: "あす", output: ""),
        ])
        XCTAssertTrue(rules.isEmpty)
        XCTAssertEqual(rules.candidates(forReading: "きょう"), [])
        XCTAssertEqual(rules.rules.count, 3)  // 一覧には残る
    }

    // MARK: - 永続化

    func testStoreRoundTripPreservesOrderAndEnabledFlag() throws {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-rewrite-\(UUID().uuidString).json")
        defer { try? FileManager.default.removeItem(at: url) }

        let store = UserRewriteRuleStore(url: url)
        XCTAssertTrue(store.current.isEmpty)
        store.replaceAll([
            UserRewriteRule(trigger: "いま", output: "{{time}}"),
            UserRewriteRule(trigger: "きょう", output: "{{date}}", isEnabled: false),
            UserRewriteRule(trigger: "", output: "捨てられる"),
        ])

        let reloaded = UserRewriteRuleStore(url: url)
        XCTAssertEqual(reloaded.rules.map(\.trigger), ["いま", "きょう"])
        XCTAssertEqual(reloaded.rules.map(\.isEnabled), [true, false])
        XCTAssertEqual(reloaded.rules.map(\.id), store.rules.map(\.id))

        reloaded.setEnabled(true, id: reloaded.rules[1].id)
        XCTAssertEqual(reloaded.current.candidates(forReading: "きょう").count, 1)
        reloaded.remove(ids: [reloaded.rules[0].id])
        XCTAssertEqual(UserRewriteRuleStore(url: url).rules.map(\.trigger), ["きょう"])
    }

    func testStoreSeedsDefaultRulesOnceWithoutDuplicatingUserRules() throws {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-rewrite-\(UUID().uuidString).json")
        defer { try? FileManager.default.removeItem(at: url) }

        // 旧ファイル（既定ルール未投入）に、ユーザが自分で「きょう」を登録している
        UserRewriteRuleStore(url: url).replaceAll([UserRewriteRule(trigger: "きょう", output: "{{date:M/d}}")])

        let seeded = UserRewriteRuleStore(url: url, seedsDefaults: true)
        XCTAssertEqual(seeded.rules.first?.output, "{{date:M/d}}", "ユーザのルールは上書きしない")
        XCTAssertEqual(seeded.rules.filter { $0.trigger == "きょう" }.count, 1)
        XCTAssertEqual(
            Set(seeded.rules.map(\.trigger)),
            Set(UserRewriteRuleStore.defaultRules.map(\.trigger)))
        XCTAssertEqual(seeded.current.candidates(forReading: "あした", context: fixedContext()), ["2026/09/07"])
        XCTAssertEqual(seeded.current.candidates(forReading: "いま", context: fixedContext()), ["14:05"])

        // 既定ルールを消しても、次の起動で復活しない
        seeded.remove(ids: Set(seeded.rules.filter { $0.trigger == "あす" }.map(\.id)))
        let again = UserRewriteRuleStore(url: url, seedsDefaults: true)
        XCTAssertFalse(again.rules.contains { $0.trigger == "あす" })
    }

    func testDecodeTolerantOfMissingOptionalFields() throws {
        let json = """
        {"id":"00000000-0000-0000-0000-000000000001","trigger":"キョウ","output":"{{date}}"}
        """
        let rule = try JSONDecoder().decode(UserRewriteRule.self, from: Data(json.utf8))
        XCTAssertEqual(rule.trigger, "きょう")
        XCTAssertEqual(rule.kind, .exact)
        XCTAssertTrue(rule.isEnabled)
    }
}
