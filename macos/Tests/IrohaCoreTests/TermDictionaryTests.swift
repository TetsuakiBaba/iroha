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
