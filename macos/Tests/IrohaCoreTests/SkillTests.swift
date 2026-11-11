import XCTest
@testable import IrohaCore

final class SkillCodeTests: XCTestCase {

    /// HMAC-SHA256 の実装（RFC 2104 のよく使われる検算値）
    func testHMAC() {
        let mac = SkillCode.hmacSHA256(
            key: Array("key".utf8), message: Array("The quick brown fox jumps over the lazy dog".utf8))
        XCTAssertEqual(
            mac.map { String(format: "%02x", $0) }.joined(),
            "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8")
    }

    /// 別の実装（Python の hmac）で求めた値と一致する。青空写経の JavaScript もこの値になること
    func testCodeMatchesReferenceValues() {
        XCTAssertEqual(SkillCode.code(id: "7QF2MX8K", work: "01"), "01XV922G")
        XCTAssertEqual(SkillCode.code(id: "7QF2MX8K", work: "02"), "02XQGSYN")
        XCTAssertEqual(SkillCode.code(id: "00000000", work: "0A"), "0A9V7Q62")
        XCTAssertEqual(SkillCode.code(id: "ZZZZZZZZ", work: "ZZ"), "ZZKE92GA")
    }

    func testRejectsMalformedIDAndWork() {
        XCTAssertNil(SkillCode.code(id: "7QF2MX8", work: "01"))
        XCTAssertNil(SkillCode.code(id: "7QF2MX8K", work: "1"))
        XCTAssertNil(SkillCode.code(id: "7QF2MX8U", work: "01"))
    }

    func testNormalize() {
        XCTAssertEqual(SkillCode.normalize(" 01-xv922g "), "01XV922G")
        XCTAssertEqual(SkillCode.normalize("０１－ＸＶ９２２Ｇ"), "01XV922G")
        // 見間違えやすい文字は数字に寄せる
        XCTAssertEqual(SkillCode.normalize("O1IL"), "0111")
        XCTAssertNil(SkillCode.normalize("01-XV92UG"))
        XCTAssertNil(SkillCode.normalize("あいう"))
    }

    func testVerify() {
        XCTAssertEqual(SkillCode.work(ofCode: "01-xv922g", id: "7QF2MX8K"), "01")
        // 別の ID のコードは通らない
        XCTAssertNil(SkillCode.work(ofCode: "01-XV922G", id: "7QF2MX8M"))
        // 作品の番号だけ替えても通らない
        XCTAssertNil(SkillCode.work(ofCode: "02-XV922G", id: "7QF2MX8K"))
        XCTAssertNil(SkillCode.work(ofCode: "01-XV922", id: "7QF2MX8K"))
    }

    func testNewIDAndFormat() {
        let id = SkillCode.newID()
        XCTAssertTrue(SkillCode.isValidID(id))
        XCTAssertEqual(SkillCode.formatID("7QF2MX8K"), "7QF2-MX8K")
        XCTAssertEqual(SkillCode.format("01XV922G"), "01-XV922G")
    }
}

final class SkillStateTests: XCTestCase {

    private let id = "7QF2MX8K"

    private func state(works: [String], id: String? = nil) -> SkillState {
        var state = SkillState(id: id ?? self.id, idCreatedAt: Date(timeIntervalSince1970: 1000))
        for (index, work) in works.enumerated() {
            let code = SkillCode.code(id: state.id!, work: work)!
            XCTAssertEqual(state.add(code: code, date: Date(timeIntervalSince1970: 2000 + Double(index))), .added(work: work))
        }
        return state
    }

    func testAddCode() {
        var state = SkillState()
        XCTAssertEqual(state.add(code: "01XV922G"), .noID)
        state = self.state(works: [])
        XCTAssertEqual(state.add(code: "01-XV922G"), .added(work: "01"))
        // 同じ作品のコードは 1 つと数える
        XCTAssertEqual(state.add(code: "01-XV922G"), .alreadyUsed(work: "01"))
        XCTAssertEqual(state.add(code: "02-XV922G"), .invalid)
        XCTAssertEqual(state.codeCount, 1)
        XCTAssertEqual(state.unusedCodeCount, 1)
    }

    func testChooseSkill() {
        var state = self.state(works: ["01", "02"])
        XCTAssertFalse(state.isUnlocked("prediction"))
        XCTAssertTrue(state.choose(skill: "prediction"))
        XCTAssertTrue(state.isUnlocked("prediction"))
        // 同じスキルを 2 回は選べない（コードを減らさない）
        XCTAssertFalse(state.choose(skill: "prediction"))
        XCTAssertEqual(state.unusedCodeCount, 1)
        XCTAssertTrue(state.choose(skill: "aiFix"))
        // 使っていないコードがなければ選べない
        XCTAssertFalse(state.choose(skill: "tapInput"))
        XCTAssertFalse(state.isUnlocked("tapInput"))
    }

    func testSevenCodesUnlockEverything() {
        var state = self.state(works: ["01", "02", "03", "04", "05", "06"])
        XCTAssertFalse(state.isAllUnlocked)
        XCTAssertFalse(state.isUnlocked("anything"))
        let seventh = SkillCode.code(id: id, work: "07")!
        XCTAssertEqual(state.add(code: seventh), .added(work: "07"))
        XCTAssertTrue(state.isAllUnlocked)
        XCTAssertTrue(state.isUnlocked("anything"))
        XCTAssertEqual(state.unusedCodeCount, 0)
    }

    func testMergeKeepsCodesFromBoth() {
        var first = state(works: ["01", "02"])
        XCTAssertTrue(first.choose(skill: "prediction", date: Date(timeIntervalSince1970: 3000)))
        var second = state(works: ["02", "03"])
        XCTAssertTrue(second.choose(skill: "aiFix", date: Date(timeIntervalSince1970: 3500)))
        let merged = first.merged(with: second)
        XCTAssertEqual(merged.entries.map(\.work).sorted(), ["01", "02", "03"])
        XCTAssertTrue(merged.isUnlocked("prediction"))
        XCTAssertTrue(merged.isUnlocked("aiFix"))
        XCTAssertEqual(merged, second.merged(with: first))
        // 合わせた結果をもう一度合わせても変わらない
        XCTAssertEqual(merged.merged(with: first), merged)
    }

    /// 2 台で同期の前に同じスキルを選んでいたら、後のほうのコードは使っていないものに戻す
    func testMergeReturnsCodeWhenSameSkillChosenTwice() {
        var first = state(works: ["01"])
        XCTAssertTrue(first.choose(skill: "prediction", date: Date(timeIntervalSince1970: 3000)))
        var second = state(works: ["02"])
        XCTAssertTrue(second.choose(skill: "prediction", date: Date(timeIntervalSince1970: 4000)))
        let merged = first.merged(with: second)
        XCTAssertEqual(merged.codeCount, 2)
        XCTAssertEqual(merged.unusedCodeCount, 1)
        XCTAssertEqual(merged.entries.first { $0.skill == "prediction" }?.work, "01")
    }

    /// 2 台が別々の ID を作っていたら、先に作ったほうにそろえる。コードは作ったときの ID で照合するので両方残る
    func testMergeWithDifferentIDs() {
        let first = state(works: ["01"])
        var second = SkillState(id: "00000000", idCreatedAt: Date(timeIntervalSince1970: 500))
        XCTAssertEqual(second.add(code: SkillCode.code(id: "00000000", work: "0A")!), .added(work: "0A"))
        let merged = first.merged(with: second)
        XCTAssertEqual(merged.id, "00000000")
        XCTAssertEqual(merged.codeCount, 2)
    }

    func testNormalizedDropsInvalidCodes() {
        var state = self.state(works: ["01"])
        state.entries.append(.init(work: "02", code: "02AAAAAA", id: id, date: Date()))
        XCTAssertEqual(state.normalized().entries.map(\.work), ["01"])
    }
}

final class SkillDeveloperCodeTests: XCTestCase {

    /// テスト用のコード（本物の開発者用コードはソースにもテストにも書かない）
    private let code = "ABCDEFGH23456789"
    private var hashes: Set<String> { [SkillCode.developerHash(of: code)] }

    func testRecognizesDeveloperCode() {
        XCTAssertTrue(SkillCode.isDeveloperCode("abcd-efgh-2345-6789", hashes: hashes))
        XCTAssertTrue(SkillCode.isDeveloperCode(SkillCode.formatDeveloperCode(code), hashes: hashes))
        XCTAssertFalse(SkillCode.isDeveloperCode("ABCD-EFGH-2345-6780", hashes: hashes))
        XCTAssertFalse(SkillCode.isDeveloperCode("ABCD-EFGH", hashes: hashes))
        XCTAssertFalse(SkillCode.isDeveloperCode(code, hashes: []))
    }

    func testHashDoesNotContainCode() {
        let hash = SkillCode.developerHash(of: code)
        XCTAssertEqual(hash.count, 64)
        XCTAssertFalse(hash.uppercased().contains(code))
    }

    func testNewDeveloperCode() {
        let code = SkillCode.newDeveloperCode()
        XCTAssertEqual(code.count, SkillCode.developerCodeLength)
        XCTAssertEqual(SkillCode.normalize(code), code)
        XCTAssertEqual(SkillCode.formatDeveloperCode("ABCDEFGH23456789"), "ABCD-EFGH-2345-6789")
    }

    /// 開発者用コードは ID が無くても受け取り、1 つですべて解除する
    func testDeveloperCodeUnlocksEverything() {
        var state = SkillState()
        XCTAssertFalse(state.isUnlocked("prediction"))
        let date = Date(timeIntervalSince1970: 1000)
        XCTAssertEqual(state.add(code: "abcd-efgh-2345-6789", date: date, developerHashes: hashes), .developer)
        XCTAssertTrue(state.isAllUnlocked)
        XCTAssertTrue(state.isDeveloperUnlocked)
        XCTAssertTrue(state.isUnlocked("prediction"))
        XCTAssertEqual(state.codeCount, 0)
        XCTAssertEqual(state.unusedCodeCount, 0)
        // もう一度入れても日時は変わらない
        XCTAssertEqual(state.add(code: code, date: Date(timeIntervalSince1970: 2000), developerHashes: hashes), .developer)
        XCTAssertEqual(state.developerUnlockedAt, date)
    }

    /// ハッシュ値を登録していない版では、ふつうのコードとして扱って通さない
    func testUnknownDeveloperCodeIsInvalid() {
        var state = SkillState(id: "7QF2MX8K", idCreatedAt: Date(timeIntervalSince1970: 1000))
        XCTAssertEqual(state.add(code: code, developerHashes: []), .invalid)
        XCTAssertFalse(state.isAllUnlocked)
    }

    func testMergeKeepsDeveloperUnlock() {
        var unlocked = SkillState()
        XCTAssertEqual(
            unlocked.add(code: code, date: Date(timeIntervalSince1970: 1000), developerHashes: hashes), .developer)
        let other = SkillState(id: "7QF2MX8K", idCreatedAt: Date(timeIntervalSince1970: 500))
        XCTAssertTrue(other.merged(with: unlocked).isDeveloperUnlocked)
        XCTAssertTrue(unlocked.merged(with: other).isDeveloperUnlocked)
        XCTAssertEqual(other.merged(with: unlocked).id, "7QF2MX8K")
    }

    /// 開発者用コードの項目がない前の版のファイルも読める
    func testDecodesStateWithoutDeveloperField() throws {
        let json = #"{"entries":[],"id":"7QF2MX8K","idCreatedAt":"2026-01-01T00:00:00Z","version":1}"#
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let state = try decoder.decode(SkillState.self, from: Data(json.utf8))
        XCTAssertFalse(state.isDeveloperUnlocked)
        XCTAssertEqual(state.id, "7QF2MX8K")
    }
}

final class SkillStoreTests: XCTestCase {

    private var directory: URL!
    private var url: URL { directory.appendingPathComponent("skills.json") }

    override func setUp() {
        directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-skill-tests-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    }

    override func tearDown() {
        try? FileManager.default.removeItem(at: directory)
    }

    func testDoesNotCreateFileUntilNeeded() {
        let store = SkillStore(url: url, mirror: nil)
        XCTAssertFalse(store.isUnlocked("prediction"))
        XCTAssertFalse(FileManager.default.fileExists(atPath: url.path))
    }

    func testPersistsAcrossInstances() {
        let store = SkillStore(url: url, mirror: nil)
        let id = store.ensureID()
        XCTAssertTrue(SkillCode.isValidID(id))
        XCTAssertEqual(store.ensureID(), id)
        XCTAssertEqual(store.addCode(SkillCode.code(id: id, work: "01")!), .added(work: "01"))
        XCTAssertTrue(store.choose(skill: "prediction"))

        let reopened = SkillStore(url: url, mirror: nil)
        XCTAssertEqual(reopened.state, store.state)
        XCTAssertTrue(reopened.isUnlocked("prediction"))
        XCTAssertFalse(reopened.isUnlocked("aiFix"))
    }

    /// 別の Mac が、こちらのコードを知らないままファイルを書いても、こちらのコードは消えない
    func testKeepsOwnCodesWhenFileIsOverwritten() throws {
        let store = SkillStore(url: url, mirror: nil)
        let id = store.ensureID()
        XCTAssertEqual(store.addCode(SkillCode.code(id: id, work: "01")!), .added(work: "01"))

        var other = SkillState(id: id, idCreatedAt: store.state.idCreatedAt)
        XCTAssertEqual(
            other.add(code: SkillCode.code(id: id, work: "02")!, date: Date(timeIntervalSince1970: 5000)),
            .added(work: "02"))
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        try encoder.encode(other).write(to: url, options: .atomic)
        // 更新日時が同じ秒になって変更を見落とさないよう、更新日時をずらす
        try FileManager.default.setAttributes(
            [.modificationDate: Date().addingTimeInterval(10)], ofItemAtPath: url.path)

        XCTAssertTrue(store.reloadIfChanged())
        XCTAssertEqual(store.state.entries.map(\.work).sorted(), ["01", "02"])
        // 合わせた結果をファイルにも書き戻している
        XCTAssertEqual(SkillStore(url: url, mirror: nil).state.codeCount, 2)
    }

    /// この Mac が覚えている状態（UserDefaults の写し）から、ファイルが消えても戻せる
    func testRestoresFromMirror() throws {
        let suite = "iroha-skill-tests-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }

        let store = SkillStore(url: url, mirror: defaults)
        let id = store.ensureID()
        XCTAssertEqual(store.addCode(SkillCode.code(id: id, work: "01")!), .added(work: "01"))
        try FileManager.default.removeItem(at: url)

        let reopened = SkillStore(url: url, mirror: defaults)
        XCTAssertEqual(reopened.state.id, id)
        XCTAssertEqual(reopened.state.codeCount, 1)
        XCTAssertTrue(FileManager.default.fileExists(atPath: url.path))
    }
}
