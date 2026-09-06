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
