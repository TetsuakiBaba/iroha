import XCTest
@testable import IrohaCore

final class UserDictionaryTests: XCTestCase {

    private func makeDictionary(_ pairs: [(String, String)]) -> UserDictionary {
        UserDictionary(entries: pairs.map { UserDictionaryEntry(reading: $0.0, word: $0.1) })
    }

    // MARK: - 読みの正規化・取り込み判定

    func testNormalizedReadingConvertsKatakanaAndTrims() {
        XCTAssertEqual(UserDictionary.normalizedReading(" キララザカ "), "きららざか")
        XCTAssertEqual(UserDictionary.normalizedReading("きららざか"), "きららざか")
    }

    func testIsImportableReadingAcceptsKanaOnly() {
        XCTAssertTrue(UserDictionary.isImportableReading("きららざか"))
        XCTAssertTrue(UserDictionary.isImportableReading("キララザカ"))
        XCTAssertTrue(UserDictionary.isImportableReading("こーひー"))
        XCTAssertFalse(UserDictionary.isImportableReading("omw"))
        XCTAssertFalse(UserDictionary.isImportableReading("雲母坂"))
        XCTAssertFalse(UserDictionary.isImportableReading(""))
    }

    // MARK: - 索引

    func testWordsForReadingKeepsRegistrationOrder() {
        let dictionary = makeDictionary([("あきら", "彰"), ("あきら", "章"), ("きららざか", "雲母坂")])
        XCTAssertEqual(dictionary.words(forReading: "あきら"), ["彰", "章"])
        // カタカナで引いてもひらがなに正規化される
        XCTAssertEqual(dictionary.words(forReading: "キララザカ"), ["雲母坂"])
        XCTAssertEqual(dictionary.words(forReading: "しらない"), [])
    }

    // MARK: - 分割（最長一致）

    func testSplitLeavesReadingIntactWhenNothingMatches() {
        let dictionary = makeDictionary([("きららざか", "雲母坂")])
        XCTAssertEqual(dictionary.split("きょうはいいてんき"), [.reading("きょうはいいてんき")])
    }

    func testSplitReplacesMatchedPart() {
        let dictionary = makeDictionary([("きららざか", "雲母坂")])
        XCTAssertEqual(
            dictionary.split("きららざかにいく"),
            [.word("雲母坂"), .reading("にいく")])
        XCTAssertEqual(
            dictionary.split("あすきららざかへ"),
            [.reading("あす"), .word("雲母坂"), .reading("へ")])
    }

    func testSplitPrefersLongestMatch() {
        let dictionary = makeDictionary([("とうきょう", "東京"), ("とうきょうと", "東京都")])
        XCTAssertEqual(dictionary.split("とうきょうとに"), [.word("東京都"), .reading("に")])
    }

    func testSplitIgnoresSingleCharacterEntriesInsideSentence() {
        // 1文字の読みが文中で無差別に一致すると変換が壊れるため、部分一致からは外す
        let dictionary = makeDictionary([("あ", "阿")])
        XCTAssertEqual(dictionary.split("あさになった"), [.reading("あさになった")])
        // 完全一致は長さに関係なく拾える
        XCTAssertEqual(dictionary.words(forReading: "あ"), ["阿"])
    }

    // MARK: ライブ変換への適用可否（単語が読みより大幅に長いものは候補ウィンドウ専用）

    func testLiveConversionSuitabilityLooksOnlyAtMuchLongerWords() {
        typealias D = UserDictionary
        // 短くなる方向（漢字変換）は常に通す
        XCTAssertTrue(D.isSuitableForLiveConversion(reading: "とうきょうとりつだいがく", word: "東京都立大学"))
        XCTAssertTrue(D.isSuitableForLiveConversion(reading: "しょう", word: "賞"))
        XCTAssertTrue(D.isSuitableForLiveConversion(reading: "まる", word: "○"))
        // 同程度の長さ・軽い略記は通す
        XCTAssertTrue(D.isSuitableForLiveConversion(reading: "あいふぉん", word: "iPhone"))
        XCTAssertTrue(D.isSuitableForLiveConversion(reading: "はっしゅたぐ", word: "#iroha"))
        XCTAssertTrue(D.isSuitableForLiveConversion(reading: "かぶ", word: "株式会社"), "2倍ちょうど・差2は通す")
        // 大幅に長い（2倍超かつ3文字以上）は除外
        XCTAssertFalse(D.isSuitableForLiveConversion(reading: "たぐ", word: "#helloworld #dummytagu"))
        XCTAssertFalse(D.isSuitableForLiveConversion(reading: "たぐ", word: "#hello"))
        XCTAssertFalse(D.isSuitableForLiveConversion(reading: "めーる", word: "someone@example.com"))
        XCTAssertFalse(D.isSuitableForLiveConversion(reading: "おせわ", word: "お世話になっております。"))
    }

    func testLiveWordsExcludeCandidateOnlyEntries() {
        let dictionary = makeDictionary([
            ("たぐ", "#helloworld #dummytagu"), ("たぐ", "タグ"), ("きららざか", "雲母坂"),
        ])
        XCTAssertEqual(dictionary.words(forReading: "たぐ"), ["#helloworld #dummytagu", "タグ"],
                       "候補ウィンドウ用には全部残る")
        XCTAssertEqual(dictionary.liveWords(forReading: "たぐ"), ["タグ"])
        XCTAssertEqual(dictionary.liveWords(forReading: "きららざか"), ["雲母坂"])
        XCTAssertFalse(dictionary.isEmptyForLiveConversion)
    }

    func testDictionaryWithOnlyCandidateOnlyEntriesIsEmptyForLiveConversion() {
        let dictionary = makeDictionary([("たぐ", "#helloworld #dummytagu")])
        XCTAssertFalse(dictionary.isEmpty)
        XCTAssertTrue(dictionary.isEmptyForLiveConversion)
        XCTAssertEqual(dictionary.liveWords(forReading: "たぐ"), [])
    }

    func testSplitForLiveConversionSkipsCandidateOnlyEntries() {
        let dictionary = makeDictionary([("たぐ", "#helloworld #dummytagu"), ("きららざか", "雲母坂")])
        XCTAssertEqual(dictionary.split("たぐをつける"), [.word("#helloworld #dummytagu"), .reading("をつける")])
        XCTAssertEqual(dictionary.split("たぐをつける", forLiveConversion: true), [.reading("たぐをつける")])
        XCTAssertEqual(dictionary.split("きららざかのたぐ", forLiveConversion: true),
                       [.word("雲母坂"), .reading("のたぐ")])
    }

    func testCandidateOnlyWordsMatchWholeOrPartialReading() {
        let dictionary = makeDictionary([
            ("たぐ", "#helloworld #dummytagu"), ("きららざか", "雲母坂"), ("め", "someone@example.com"),
        ])
        XCTAssertEqual(dictionary.candidateOnlyWords(in: "たぐ"), ["#helloworld #dummytagu"])
        XCTAssertEqual(dictionary.candidateOnlyWords(in: "たぐをつける"), ["#helloworld #dummytagu"])
        XCTAssertEqual(dictionary.candidateOnlyWords(in: "きららざか"), [], "ライブ変換に使う語は含まない")
        XCTAssertEqual(dictionary.candidateOnlyWords(in: "め"), ["someone@example.com"], "完全一致は1文字でも拾う")
        XCTAssertEqual(dictionary.candidateOnlyWords(in: "めがね"), [], "1文字の部分一致は拾わない")
        XCTAssertEqual(UserDictionary.empty.candidateOnlyWords(in: "たぐ"), [])
    }

    func testEmptyDictionaryDoesNotSplit() {
        XCTAssertEqual(UserDictionary.empty.split("きららざか"), [.reading("きららざか")])
        XCTAssertTrue(UserDictionary.empty.isEmpty)
    }
}

// MARK: - デコレータ

/// 呼び出しを記録するだけのダミーエンジン（読みをカタカナにして返す）
private final class StubEngine: ConversionEngine, @unchecked Sendable {
    private(set) var calls: [(reading: String, context: String, count: Int)] = []
    var shouldFail = false

    func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        calls.append((reading, context, candidateCount))
        if shouldFail { throw ConversionError.inferenceFailed("テスト") }
        return (0..<max(1, candidateCount)).map { index in
            index == 0 ? hiraganaToKatakana(reading) : "\(hiraganaToKatakana(reading))\(index)"
        }
    }
}

final class UserDictionaryEngineTests: XCTestCase {

    private func makeEngine(_ pairs: [(String, String)]) -> (UserDictionaryEngine, StubEngine) {
        let stub = StubEngine()
        let dictionary = UserDictionary(
            entries: pairs.map { UserDictionaryEntry(reading: $0.0, word: $0.1) })
        return (UserDictionaryEngine(base: stub, dictionary: { dictionary }), stub)
    }

    func testEmptyDictionaryPassesThrough() async throws {
        let (engine, stub) = makeEngine([])
        let result = try await engine.convert(reading: "てすと", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["テスト"])
        XCTAssertEqual(stub.calls.count, 1)
        XCTAssertEqual(stub.calls[0].reading, "てすと")
    }

    func testExactMatchSkipsEngine() async throws {
        let (engine, stub) = makeEngine([("きららざか", "雲母坂")])
        let result = try await engine.convert(reading: "きららざか", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["雲母坂"])
        XCTAssertTrue(stub.calls.isEmpty, "完全一致ならエンジンを呼ばない")
    }

    func testPartialMatchComposesWithEngine() async throws {
        let (engine, stub) = makeEngine([("きららざか", "雲母坂")])
        let result = try await engine.convert(reading: "きららざかにいく", context: "昨日", candidateCount: 1)
        XCTAssertEqual(result, ["雲母坂ニイク"])
        // 残りの読みだけがエンジンに渡り、確定済みの単語が文脈として付く
        XCTAssertEqual(stub.calls.count, 1)
        XCTAssertEqual(stub.calls[0].reading, "にいく")
        XCTAssertEqual(stub.calls[0].context, "昨日雲母坂")
    }

    func testUnmatchedReadingIsConvertedInOneCall() async throws {
        let (engine, stub) = makeEngine([("きららざか", "雲母坂")])
        let result = try await engine.convert(reading: "きょうはさむい", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["キョウハサムイ"])
        XCTAssertEqual(stub.calls.count, 1, "一致しなければ読み全体を1回で変換する")
    }

    func testCandidatePanelPutsUserWordsFirst() async throws {
        let (engine, _) = makeEngine([("きららざか", "雲母坂"), ("きららざか", "煌坂")])
        let result = try await engine.convert(reading: "きららざか", context: "", candidateCount: 3)
        XCTAssertEqual(result.prefix(2).map { $0 }, ["雲母坂", "煌坂"])
        XCTAssertTrue(result.contains("キララザカ"), "エンジンの候補も残る")
    }

    func testUserWordsSurviveEngineFailure() async throws {
        let (engine, stub) = makeEngine([("きららざか", "雲母坂")])
        stub.shouldFail = true
        let result = try await engine.convert(reading: "きららざか", context: "", candidateCount: 5)
        XCTAssertEqual(result, ["雲母坂"])
    }

    func testCandidateOnlyWordIsNotUsedForLiveConversion() async throws {
        let (engine, stub) = makeEngine([("たぐ", "#helloworld #dummytagu")])
        let live = try await engine.convert(reading: "たぐ", context: "", candidateCount: 1)
        XCTAssertEqual(live, ["タグ"], "ライブ変換は素通しでエンジンの結果")
        XCTAssertEqual(stub.calls.count, 1)
        XCTAssertEqual(stub.calls[0].reading, "たぐ")

        let composed = try await engine.convert(reading: "たぐをつける", context: "", candidateCount: 1)
        XCTAssertEqual(composed, ["タグヲツケル"], "部分一致でも埋め込まない")

        let panel = try await engine.convert(reading: "たぐ", context: "", candidateCount: 3)
        XCTAssertEqual(panel.first, "#helloworld #dummytagu", "候補ウィンドウには先頭で出る")
        XCTAssertTrue(panel.contains("タグ"))
    }

    func testLiveConversionStillUsesShortWordsAlongsideCandidateOnlyOnes() async throws {
        let (engine, _) = makeEngine([("たぐ", "#helloworld #dummytagu"), ("たぐ", "タグ付け"), ("きららざか", "雲母坂")])
        let live = try await engine.convert(reading: "たぐ", context: "", candidateCount: 1)
        XCTAssertEqual(live, ["タグ付け"], "同じ読みでも通常の語はライブ変換に使う")
        let composed = try await engine.convert(reading: "きららざかのたぐ", context: "", candidateCount: 1)
        XCTAssertEqual(composed, ["雲母坂ノタグ付け"])
    }

    func testEngineFailurePropagatesWithoutUserWords() async {
        let (engine, stub) = makeEngine([("きららざか", "雲母坂")])
        stub.shouldFail = true
        do {
            _ = try await engine.convert(reading: "まったくべつ", context: "", candidateCount: 5)
            XCTFail("エンジンのエラーがそのまま伝わるべき")
        } catch {
            // 期待どおり
        }
    }
}

// MARK: - 永続化

final class UserDictionaryStoreTests: XCTestCase {

    private var url: URL!

    override func setUp() {
        super.setUp()
        url = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("iroha-test-\(UUID().uuidString).json")
    }

    override func tearDown() {
        try? FileManager.default.removeItem(at: url)
        super.tearDown()
    }

    func testAddAndReload() {
        let store = UserDictionaryStore(url: url)
        store.add(reading: "キララザカ", word: "雲母坂")
        XCTAssertEqual(store.current.words(forReading: "きららざか"), ["雲母坂"])

        // 別インスタンス（=次回起動）でも読み戻せる
        let reloaded = UserDictionaryStore(url: url)
        XCTAssertEqual(reloaded.current.words(forReading: "きららざか"), ["雲母坂"])
    }

    func testStoreDropsEmptyEntries() {
        let store = UserDictionaryStore(url: url)
        store.replaceAll([
            UserDictionaryEntry(reading: "あ", word: ""),
            UserDictionaryEntry(reading: "", word: "空"),
            UserDictionaryEntry(reading: "とうきょう", word: "東京"),
        ])
        XCTAssertEqual(store.entries.count, 1)
    }

    func testSyncFromSystemAddsRemovesAndSkips() {
        let store = UserDictionaryStore(url: url)
        var result = store.syncFromSystem([
            (reading: "きららざか", word: "雲母坂"),
            (reading: "とうきょうと", word: "東京都"),
            (reading: "omw", word: "On my way!"),
        ])
        XCTAssertEqual(result.added, 2)
        XCTAssertEqual(result.skipped, 1, "ASCIIショートカットは取り込まない")
        XCTAssertEqual(store.entries.count, 2)

        // 2回目は差分だけ: 1件消えて1件増える
        result = store.syncFromSystem([
            (reading: "きららざか", word: "雲母坂"),
            (reading: "きょうと", word: "京都"),
        ])
        XCTAssertEqual(result.added, 1)
        XCTAssertEqual(result.removed, 1)
        XCTAssertEqual(result.unchanged, 1)
        XCTAssertEqual(Set(store.entries.map(\.word)), ["雲母坂", "京都"])
    }

    func testSyncKeepsManualEntries() {
        let store = UserDictionaryStore(url: url)
        store.add(reading: "じぶん", word: "自分")
        let result = store.syncFromSystem([(reading: "きららざか", word: "雲母坂")])
        XCTAssertEqual(result.added, 1)
        XCTAssertEqual(result.removed, 0)
        XCTAssertEqual(store.current.words(forReading: "じぶん"), ["自分"],
                       "手動エントリは同期で消えない")
    }
}
