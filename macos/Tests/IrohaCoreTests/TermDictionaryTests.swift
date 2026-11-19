import XCTest
@testable import IrohaCore

/// 専門辞書: 辞書ファイルの解釈・索引・取得と設置・辞書ラティスへの加わり方
final class TermDictionaryTests: XCTestCase {

    private var root: URL!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-term-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        // 設置先が本物のデータフォルダにならないよう、テスト用の場所へ向ける
        DataDirectory.configure(root.appendingPathComponent("data", isDirectory: true))
    }

    override func tearDownWithError() throws {
        DataDirectory.configure(DataDirectory.defaultURL)
        try? FileManager.default.removeItem(at: root)
    }

    private let sampleText = """
        # iroha 専門辞書: テスト
        # 出典: テスト
        じょくそう\t褥瘡
        かんせん\t感染
        かんせん\t観戦
        あ\t亜
        カタカナ\t片仮名
        よみだけ
        abc\tエービーシー
        """

    func testParseSkipsCommentsAndInvalidLines() {
        let pairs = TermDictionary.parse(sampleText)
        XCTAssertEqual(pairs.map(\.word), ["褥瘡", "感染", "観戦", "亜", "片仮名"])
        // 読みはひらがなにそろえる（カタカナで書かれていても受け付ける）
        XCTAssertEqual(pairs.last?.reading, "カタカナ")
    }

    // MARK: - 取得と設置

    private func makeCatalog(text: String, corrupt: Bool = false) throws -> URL {
        let data = Data(text.utf8)
        let fileURL = root.appendingPathComponent("test-medicine-20261108.tsv")
        try data.write(to: fileURL)
        let json: [String: Any] = [
            "formatVersion": 1,
            "dictionaries": [[
                "id": "test-medicine",
                "name": "医学",
                "group": "分野別の用語（テスト）",
                "summary": "テスト用",
                "entries": 5,
                "kind": "noun",
                "file": [
                    "url": fileURL.absoluteString,
                    "bytes": data.count,
                    "sha256": corrupt ? String(repeating: "0", count: 64) : SHA256.hex(data),
                ],
                "license": "CC BY-SA 4.0",
                "sources": [["name": "JMdict", "holder": "EDRDG", "license": "CC BY-SA 4.0", "url": "https://example.com"]],
                "version": "20261108",
            ]],
        ]
        let catalogURL = root.appendingPathComponent("catalog.json")
        try JSONSerialization.data(withJSONObject: json).write(to: catalogURL)
        return catalogURL
    }

    func testLookupFindsDictionaryNames() {
        // build-term-dictionaries.py と同じく、読み・語の順（UTF-8 のバイト順）に並べる
        func table(_ label: String, _ pairs: [(String, String)]) -> TermDictionaryLookup.Table {
            let lines = pairs.map { "\($0.0)\t\($0.1)" }.sorted { Array($0.utf8).lexicographicallyPrecedes(Array($1.utf8)) }
            let data = Data(("# 注記\n# 形式: 読み<TAB>語\n" + lines.joined(separator: "\n") + "\n").utf8)
            return .init(label: label, data: data, start: TermDictionaryLookup.bodyStart(data))
        }
        let engineering = table("工学", [("ちらく", "地絡"), ("ちら", "散ら"), ("へんちょう", "変調"), ("ぞんで", "ゾンデ")])
        let medicine = table("医学", [("ぞんで", "ゾンデ"), ("けっしゅ", "血腫"), ("ちらくしょう", "地絡症")]
            + (0..<500).map { ("あ\($0)", "亜\($0)") })
        let lookup = TermDictionaryLookup(tables: [engineering, medicine])

        XCTAssertEqual(lookup.labels(reading: "ちらく", candidate: "地絡"), ["工学"])
        // 語の後ろに助詞などのかなが続く候補
        XCTAssertEqual(lookup.labels(reading: "ちらくが", candidate: "地絡が"), ["工学"])
        XCTAssertEqual(lookup.labels(reading: "ぞんでを", candidate: "ゾンデを"), ["工学", "医学"])
        XCTAssertEqual(lookup.labels(reading: "けっしゅ", candidate: "血腫"), ["医学"])
        XCTAssertEqual(lookup.labels(reading: "あ123", candidate: "亜123"), ["医学"])
        // 辞書に無い語・読みの違う語・語の後ろがかなでない候補には付けない
        XCTAssertEqual(lookup.labels(reading: "ちらく", candidate: "散楽"), [])
        XCTAssertEqual(lookup.labels(reading: "へんちょう", candidate: "偏重"), [])
        XCTAssertEqual(lookup.labels(reading: "ちらくが", candidate: "地絡画"), [])
        XCTAssertEqual(TermDictionaryLookup.empty.labels(reading: "ちらく", candidate: "地絡"), [])
        XCTAssertEqual(TermDictionaryLookup.label(for: "地名（市区町村・町域）"), "地名")
        XCTAssertEqual(TermDictionaryLookup.label(for: "工学"), "工学")
    }

    func testSamplesAreOptional() throws {
        let catalogURL = try makeCatalog(text: sampleText)
        var json = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: catalogURL)) as? [String: Any])
        var dictionaries = try XCTUnwrap(json["dictionaries"] as? [[String: Any]])
        // 例の無い前のカタログも読める
        XCTAssertNil(try JSONDecoder().decode(TermDictionaryCatalog.self,
            from: JSONSerialization.data(withJSONObject: json)).dictionaries[0].samples)
        dictionaries[0]["samples"] = [["word": "心筋梗塞", "reading": "しんきんこうそく"]]
        json["dictionaries"] = dictionaries
        let catalog = try JSONDecoder().decode(TermDictionaryCatalog.self, from: JSONSerialization.data(withJSONObject: json))
        XCTAssertEqual(catalog.dictionaries[0].samples, [.init(word: "心筋梗塞", reading: "しんきんこうそく")])
    }

    func testInstallLoadAndRemove() async throws {
        let catalog = try await TermDictionaryFetcher.fetchCatalog(from: try makeCatalog(text: sampleText))
        XCTAssertEqual(catalog.groups.map(\.name), ["分野別の用語（テスト）"])
        let entry = try XCTUnwrap(catalog.dictionary(id: "test-medicine"))

        let record = try await TermDictionaryFetcher.install(entry)
        XCTAssertEqual(record.id, "test-medicine")
        XCTAssertEqual(TermDictionaryInstall.installedRecords().map(\.id), ["test-medicine"])
        XCTAssertFalse(TermDictionaryInstall.needsUpdate(record, to: entry))

        guard let dictionaryURL = LatticeConverter.defaultDictionaryURL() else {
            throw XCTSkip("辞書が無いためスキップ（macos/scripts/fetch-dictionary.sh で取得）")
        }
        let cache = root.appendingPathComponent("cache", isDirectory: true)
        let store = TermDictionaryStore(cacheURL: cache, dictionaryURL: { dictionaryURL })
        XCTAssertTrue(store.reload())
        let exported = try XCTUnwrap(store.current.latticeDirectory)
        // 1文字の読み（あ → 亜）は入れない
        XCTAssertEqual(store.current.entryCount, 4)
        XCTAssertTrue(FileManager.default.fileExists(atPath: exported.appendingPathComponent("user.louds").path))
        XCTAssertEqual(store.installedRecords.first?.sources.first?.name, "JMdict")
        // 中身が変わっていなければ読み直さない。別のインスタンス（次の起動）は書き出しを使い回す
        XCTAssertFalse(store.reload())
        let next = TermDictionaryStore(cacheURL: cache, dictionaryURL: { dictionaryURL })
        XCTAssertTrue(next.reload())
        XCTAssertEqual(next.current, store.current)

        try TermDictionaryInstall.remove(id: "test-medicine")
        XCTAssertTrue(TermDictionaryInstall.installedRecords().isEmpty)
        XCTAssertTrue(store.reload())
        XCTAssertTrue(store.current.isEmpty)
        XCTAssertFalse(FileManager.default.fileExists(atPath: exported.path))
    }

    func testCorruptDownloadIsNotInstalled() async throws {
        let catalog = try await TermDictionaryFetcher.fetchCatalog(
            from: try makeCatalog(text: sampleText, corrupt: true))
        let entry = try XCTUnwrap(catalog.dictionaries.first)
        do {
            try await TermDictionaryFetcher.install(entry)
            XCTFail("照合に失敗するはず")
        } catch let error as TermDictionaryFetcher.FetchError {
            XCTAssertEqual(error, .checksumMismatch)
        }
        XCTAssertTrue(TermDictionaryInstall.installedRecords().isEmpty)
    }

    func testIDMustBeSafeForFileNames() {
        XCTAssertTrue(TermDictionaryInstall.isValidID("jmdict-medicine"))
        XCTAssertFalse(TermDictionaryInstall.isValidID("../escape"))
        XCTAssertFalse(TermDictionaryInstall.isValidID("Upper"))
        XCTAssertFalse(TermDictionaryInstall.isValidID(""))
    }

    // MARK: - 辞書ラティス

    /// 専門辞書の語は、候補を作るときにラティスに加わる（azooKey の辞書に無い語・頭の部分も無い語でも候補に出る）
    func testLatticeOffersTermDictionaryWords() async throws {
        guard let url = LatticeConverter.defaultDictionaryURL() else {
            throw XCTSkip("辞書が無いためスキップ（macos/scripts/fetch-dictionary.sh で取得）")
        }
        let directory = root.appendingPathComponent("lattice", isDirectory: true)
        let count = try TermDictionary.export(
            packs: [(.noun, [("もげもげざい", "藻毛藻毛剤")]), (.person, [("ぬぬぬぬたろう", "奴奴奴奴太郎")])],
            dictionaryURL: url, to: directory)
        XCTAssertEqual(count, 2)
        let terms = TermDictionary(latticeDirectory: directory, entryCount: count)
        let plain = LatticeConverter(dictionaryURL: url)
        let withTerms = LatticeConverter(dictionaryURL: url) { terms }

        let without = await plain.fullMatchCandidates(reading: "もげもげざい", nBest: 10)
        XCTAssertFalse(without.contains("藻毛藻毛剤"))
        let with = await withTerms.fullMatchCandidates(reading: "もげもげざい", nBest: 10)
        XCTAssertTrue(with.contains("藻毛藻毛剤"), "\(with)")
        // 助詞が続いても、語として前後とつながる
        let sentence = await withTerms.fullMatchCandidates(reading: "もげもげざいをのむ", nBest: 10)
        XCTAssertTrue(sentence.contains { $0.hasPrefix("藻毛藻毛剤") }, "\(sentence)")
        let person = await withTerms.fullMatchCandidates(reading: "ぬぬぬぬたろうさん", nBest: 10)
        XCTAssertTrue(person.contains { $0.hasPrefix("奴奴奴奴太郎") }, "\(person)")
        // ユーザ辞書の語の境目の判定には専門辞書を入れない
        let boundaries = await withTerms.wordBoundaries(reading: "もげもげざい", userWords: [])
        let plainBoundaries = await plain.wordBoundaries(reading: "もげもげざい", userWords: [])
        XCTAssertEqual(boundaries, plainBoundaries)
    }
}

// MARK: - ライブ変換にも使う

/// 読みをカタカナにして返すだけのエンジン
private struct KatakanaEngine: ConversionEngine {
    func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        (0..<max(1, candidateCount)).map { $0 == 0 ? hiraganaToKatakana(reading) : "\(hiraganaToKatakana(reading))\($0)" }
    }
}

final class TermDictionaryLiveTests: XCTestCase {

    /// build-term-dictionaries.py と同じく、読み・語の順（UTF-8 のバイト順）に並べる
    private func table(_ label: String, _ pairs: [(String, String)]) -> TermDictionaryLookup.Table {
        let lines = pairs.map { "\($0.0)\t\($0.1)" }.sorted { Array($0.utf8).lexicographicallyPrecedes(Array($1.utf8)) }
        let data = Data(("# 注記\n# 形式: 読み<TAB>語\n" + lines.joined(separator: "\n") + "\n").utf8)
        return .init(label: label, data: data, start: TermDictionaryLookup.bodyStart(data))
    }

    private var lookup: TermDictionaryLookup {
        let medicine = table("医学", [
            ("いがん", "胃がん"), ("いがん", "胃癌"), ("けっしゅ", "血腫"), ("ぞんで", "ゾンデ"),
            ("けっしゅう", "結集"), ("あ", "亜"),
        ] + (0..<300).map { ("か\($0)", "火\($0)") })
        let omamori = table("お守り辞書", [
            ("げんき", "元氣"), ("がんばる", "顔晴る"), ("ぞんで", "ゾンデ"), ("めーるあどれす", "abc@example.com"),
        ])
        return TermDictionaryLookup(tables: [medicine, omamori])
    }

    func testLiveDictionaryUsesOnlyUnambiguousReadings() {
        let dictionary = lookup.liveDictionary(occurringIn: "いがんとけっしゅうとげんき")
        // 表記が分かれる読み（いがん）は使わない
        XCTAssertEqual(dictionary.liveWords(forReading: "いがん"), [])
        XCTAssertEqual(dictionary.liveWords(forReading: "けっしゅ"), ["血腫"])
        XCTAssertEqual(dictionary.liveWords(forReading: "けっしゅう"), ["結集"])
        XCTAssertEqual(dictionary.liveWords(forReading: "げんき"), ["元氣"])
        // 1 文字の読みは使わない
        XCTAssertEqual(dictionary.liveWords(forReading: "あ"), [])
        // 2 つの辞書に同じ語があっても 1 語として数える
        XCTAssertEqual(lookup.liveDictionary(occurringIn: "ぞんで").liveWords(forReading: "ぞんで"), ["ゾンデ"])
        // 読みより大幅に長い語は使わない（ユーザ辞書と同じ）
        XCTAssertTrue(lookup.liveDictionary(occurringIn: "めーるあどれす").isEmpty)
        // 読みに語が現れなければ空
        XCTAssertTrue(lookup.liveDictionary(occurringIn: "きょうはいいてんき").isEmpty)
        XCTAssertTrue(TermDictionaryLookup.empty.liveDictionary(occurringIn: "げんき").isEmpty)
        // 読みの途中に並ぶ数字の語（二分探索の範囲の確認）
        XCTAssertEqual(lookup.liveDictionary(occurringIn: "か123の").liveWords(forReading: "か123"), ["火123"])
    }

    func testEngineReplacesLikeUserDictionary() async throws {
        let lookup = self.lookup
        let engine = TermDictionaryLiveEngine(base: KatakanaEngine(), lookup: { lookup })
        // 読み全体の一致は置き換える
        let exact = try await engine.convert(reading: "げんき", context: "", candidateCount: 1)
        XCTAssertEqual(exact, ["元氣"])
        // 途中の一致は語で埋め、残りをエンジンに渡す
        let partial = try await engine.convert(reading: "げんきです", context: "", candidateCount: 1)
        XCTAssertEqual(partial, ["元氣デス"])
        // 表記が分かれる読みは置き換えない
        let ambiguous = try await engine.convert(reading: "いがん", context: "", candidateCount: 1)
        XCTAssertEqual(ambiguous, ["イガン"])
        // 候補ウィンドウでは辞書の語を先頭に出す
        let candidates = try await engine.convert(reading: "がんばる", context: "", candidateCount: 3)
        XCTAssertEqual(candidates.first, "顔晴る")
        XCTAssertTrue(candidates.contains("ガンバル"))
        // 辞書が無ければ素通し
        let none = TermDictionaryLiveEngine(base: KatakanaEngine(), lookup: { .empty })
        let passed = try await none.convert(reading: "げんき", context: "", candidateCount: 1)
        XCTAssertEqual(passed, ["ゲンキ"])
    }

    func testUserDictionaryWinsOverTermDictionary() async throws {
        let user = UserDictionary(entries: [UserDictionaryEntry(reading: "げんき", word: "ゲンキ堂")])
        let lookup = self.lookup
        let engine = UserDictionaryEngine(
            base: TermDictionaryLiveEngine(base: KatakanaEngine(), lookup: { lookup }),
            dictionary: { user })
        let result = try await engine.convert(reading: "げんき", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["ゲンキ堂"])
        let candidates = try await engine.convert(reading: "げんき", context: "", candidateCount: 3)
        XCTAssertEqual(Array(candidates.prefix(2)), ["ゲンキ堂", "元氣"])
    }
}
