import XCTest
@testable import IrohaCore

/// モデルの取得・検証・設置。`file://` のカタログで実際に最後まで通す
/// （ネットワークに出ずに、カタログ解釈 → ダウンロード → SHA-256照合 → 設置 を同じ経路で確かめる）
final class TypoNormalizerFetcherTests: XCTestCase {

    private var root: URL!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-fetch-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        // 設置先が本物のデータフォルダにならないよう、テスト用の場所へ向ける
        DataDirectory.configure(root.appendingPathComponent("data", isDirectory: true))
    }

    override func tearDownWithError() throws {
        DataDirectory.configure(DataDirectory.defaultURL)
        try? FileManager.default.removeItem(at: root)
    }

    /// 配る2ファイルを作り、それを指すカタログを書いて返す
    private func makeCatalogFile(
        manifest: Data, weights: Data, corruptChecksum: Bool = false, wrongSize: Bool = false,
        withSources: Bool = false
    ) throws -> URL {
        let manifestURL = root.appendingPathComponent("small-manifest.json")
        let weightsURL = root.appendingPathComponent("small-weights.bin")
        try manifest.write(to: manifestURL)
        try weights.write(to: weightsURL)
        let weightsHash = corruptChecksum
            ? String(repeating: "0", count: 64) : SHA256.hex(weights)
        var model: [String: Any] = [
            "id": "test-v1",
            "name": "テスト",
            "summary": "テスト用",
            "manifest": [
                "url": manifestURL.absoluteString,
                "bytes": manifest.count,
                "sha256": SHA256.hex(manifest),
            ],
            "weights": [
                "url": weightsURL.absoluteString,
                "bytes": wrongSize ? weights.count + 1 : weights.count,
                "sha256": weightsHash,
            ],
        ]
        if withSources {
            model["sources"] = [
                ["name": "コーパスA", "holder": "作者A", "license": "CC BY 4.0", "url": "https://example.com/a"],
                ["name": "コーパスB", "holder": "作者B", "license": "MIT License", "url": "https://example.com/b"],
            ]
            model["page"] = "https://example.com/model"
        }
        let json: [String: Any] = [
            "formatVersion": 1,
            "license": "CC BY-SA 4.0",
            "attribution": "テスト",
            "models": [model],
        ]
        let catalogURL = root.appendingPathComponent("catalog.json")
        try JSONSerialization.data(withJSONObject: json, options: .prettyPrinted).write(to: catalogURL)
        return catalogURL
    }

    private func sampleFiles() -> (manifest: Data, weights: Data) {
        let manifest = Data(#"{"dtype":"float16-le"}"#.utf8)
        let weights = Data((0..<40_000).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ 3) })
        return (manifest, weights)
    }

    func testFetchCatalog() async throws {
        let files = sampleFiles()
        let catalogURL = try makeCatalogFile(manifest: files.manifest, weights: files.weights)
        let catalog = try await TypoNormalizerFetcher.fetchCatalog(from: catalogURL)
        XCTAssertEqual(catalog.models.count, 1)
        let model = try XCTUnwrap(catalog.models.first)
        XCTAssertEqual(model.id, "test-v1")
        XCTAssertEqual(model.totalBytes, files.manifest.count + files.weights.count)
        XCTAssertEqual(catalog.license(for: model), "CC BY-SA 4.0")
    }

    /// 取得 → 照合 → 設置。設置後は `TypoNormalizer` が探す場所に揃って入っている
    func testInstallPlacesBothFiles() async throws {
        let files = sampleFiles()
        let catalogURL = try makeCatalogFile(manifest: files.manifest, weights: files.weights)
        let catalog = try await TypoNormalizerFetcher.fetchCatalog(from: catalogURL)
        let model = try XCTUnwrap(catalog.models.first)

        XCTAssertFalse(TypoNormalizerInstall.isInstalled)
        let progress = ProgressCollector()
        let record = try await TypoNormalizerFetcher.install(model) { progress.append($0) }

        XCTAssertTrue(TypoNormalizerInstall.isInstalled)
        XCTAssertEqual(record.id, "test-v1")
        XCTAssertEqual(TypoNormalizerInstall.installedRecord()?.id, "test-v1")
        let directory = TypoNormalizerInstall.directoryURL
        XCTAssertEqual(try Data(contentsOf: directory.appendingPathComponent("weights.bin")), files.weights)
        XCTAssertEqual(
            try Data(contentsOf: directory.appendingPathComponent("manifest.json")), files.manifest)
        XCTAssertEqual(progress.values.last, 1)
        // 同じものが入っているので入れ直しは要らない
        XCTAssertFalse(TypoNormalizerInstall.needsUpdate(to: model))
    }

    /// 学習元（sources）とライセンスは設置記録にも残す（オフラインでも 設定 > 情報 に出すため）。
    /// sources の無い古いカタログ・記録もそのまま読める
    func testInstallRecordsLicenseAndSources() async throws {
        let files = sampleFiles()
        let catalogURL = try makeCatalogFile(
            manifest: files.manifest, weights: files.weights, withSources: true)
        let catalog = try await TypoNormalizerFetcher.fetchCatalog(from: catalogURL)
        let model = try XCTUnwrap(catalog.models.first)
        XCTAssertEqual(model.sources?.map(\.name), ["コーパスA", "コーパスB"])

        _ = try await TypoNormalizerFetcher.install(model, license: catalog.license(for: model))
        let record = try XCTUnwrap(TypoNormalizerInstall.installedRecord())
        XCTAssertEqual(record.license, "CC BY-SA 4.0")   // モデルに無いのでカタログ既定
        XCTAssertEqual(record.sources?.last?.license, "MIT License")
        XCTAssertEqual(record.page, "https://example.com/model")

        // 項目を足す前の記録（id・name・sha256・installedAt だけ）
        let old = #"{"id":"old-v1","name":"標準","sha256":"00","installedAt":"2026-09-22T00:00:00Z"}"#
        try Data(old.utf8).write(to: TypoNormalizerInstall.recordURL)
        let oldRecord = try XCTUnwrap(TypoNormalizerInstall.installedRecord())
        XCTAssertEqual(oldRecord.id, "old-v1")
        XCTAssertNil(oldRecord.sources)
    }

    /// チェックサムが合わなければ設置しない（壊れたモデルを絶対に置かない）
    func testChecksumMismatchDoesNotInstall() async throws {
        let files = sampleFiles()
        let catalogURL = try makeCatalogFile(
            manifest: files.manifest, weights: files.weights, corruptChecksum: true)
        let catalog = try await TypoNormalizerFetcher.fetchCatalog(from: catalogURL)
        let model = try XCTUnwrap(catalog.models.first)
        do {
            _ = try await TypoNormalizerFetcher.install(model)
            XCTFail("チェックサム不一致なのに成功した")
        } catch let error as TypoNormalizerFetcher.FetchError {
            XCTAssertEqual(error, .checksumMismatch(name: "weights.bin"))
        }
        XCTAssertFalse(TypoNormalizerInstall.isInstalled)
    }

    /// 大きさが違っても設置しない
    func testSizeMismatchDoesNotInstall() async throws {
        let files = sampleFiles()
        let catalogURL = try makeCatalogFile(
            manifest: files.manifest, weights: files.weights, wrongSize: true)
        let catalog = try await TypoNormalizerFetcher.fetchCatalog(from: catalogURL)
        let model = try XCTUnwrap(catalog.models.first)
        do {
            _ = try await TypoNormalizerFetcher.install(model)
            XCTFail("大きさ不一致なのに成功した")
        } catch let error as TypoNormalizerFetcher.FetchError {
            guard case .sizeMismatch = error else { return XCTFail("別のエラー: \(error)") }
        }
        XCTAssertFalse(TypoNormalizerInstall.isInstalled)
    }

    /// 入れ直しの判定と削除
    func testNeedsUpdateAndRemove() async throws {
        let files = sampleFiles()
        let catalogURL = try makeCatalogFile(manifest: files.manifest, weights: files.weights)
        let catalog = try await TypoNormalizerFetcher.fetchCatalog(from: catalogURL)
        let model = try XCTUnwrap(catalog.models.first)
        _ = try await TypoNormalizerFetcher.install(model)

        // 重みが変わった（= 別のsha256）カタログなら入れ直しが要る
        let newer = try makeCatalogFile(
            manifest: files.manifest, weights: files.weights + Data([0xFF]))
        let newerCatalog = try await TypoNormalizerFetcher.fetchCatalog(from: newer)
        let newerModel = try XCTUnwrap(newerCatalog.models.first)
        XCTAssertTrue(TypoNormalizerInstall.needsUpdate(to: newerModel))

        try TypoNormalizerInstall.remove()
        XCTAssertFalse(TypoNormalizerInstall.isInstalled)
        XCTAssertNil(TypoNormalizerInstall.installedRecord())
        XCTAssertTrue(TypoNormalizerInstall.needsUpdate(to: model))
    }

    /// 形式が新しすぎるカタログは読まない（古いアプリが壊れた解釈をしないため）
    func testUnsupportedCatalogVersion() async throws {
        let catalogURL = root.appendingPathComponent("future.json")
        try Data(#"{"formatVersion": 99, "models": []}"#.utf8).write(to: catalogURL)
        do {
            _ = try await TypoNormalizerFetcher.fetchCatalog(from: catalogURL)
            XCTFail("読めてしまった")
        } catch let error as TypoNormalizerFetcher.FetchError {
            XCTAssertEqual(error, .unsupportedCatalog(99))
        }
    }

    /// 進捗コールバックは別スレッドから来るので、まとめる側で守る
    private final class ProgressCollector: @unchecked Sendable {
        private let lock = NSLock()
        private var storage: [Double] = []

        func append(_ value: Double) {
            lock.lock()
            storage.append(value)
            lock.unlock()
        }

        var values: [Double] {
            lock.lock()
            defer { lock.unlock() }
            return storage
        }
    }
}
