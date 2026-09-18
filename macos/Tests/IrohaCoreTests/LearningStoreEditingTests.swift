import Foundation
import XCTest
@testable import IrohaCore

/// 設定画面から学習内容を編集する経路（`replaceAll`）のテスト
final class LearningStoreEditingTests: XCTestCase {

    private var url: URL!

    override func setUpWithError() throws {
        url = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-learning-\(UUID().uuidString).json")
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: url)
    }

    /// 置き換えた内容が変換に使われ、ファイルにも残る
    func testReplaceAllUpdatesDictionaryAndFile() throws {
        let store = LearningStore(url: url)
        store.record(reading: "きしゃ", result: "汽車")
        XCTAssertEqual(store.current.result(forReading: "きしゃ"), "汽車")

        // 覚え違いを直す（「汽車」→「貴社」）
        var entries = store.current.entries
        for index in entries.indices where entries[index].result == "汽車" {
            entries[index].result = "貴社"
        }
        store.replaceAll(entries)

        XCTAssertTrue(store.current.entries.contains { $0.result == "貴社" })
        XCTAssertFalse(store.current.entries.contains { $0.result == "汽車" })

        // 別インスタンスで読み直しても直っている（保存は非同期なので待つ）
        let deadline = Date().addingTimeInterval(2)
        while Date() < deadline {
            let saved = (try? Data(contentsOf: url)).map { String(decoding: $0, as: UTF8.self) } ?? ""
            if saved.contains("貴社"), !saved.contains("汽車") { break }
            usleep(20_000)
        }
        let reloaded = LearningStore(url: url)
        XCTAssertTrue(reloaded.current.entries.contains { $0.result == "貴社" })
    }

    /// 読みや結果が空になった行は落とす（編集中の空欄をそのまま保存しない）
    func testReplaceAllDropsEmptyEntries() throws {
        let store = LearningStore(url: url)
        store.replaceAll([
            LearningEntry(reading: "きしゃ", result: "貴社"),
            LearningEntry(reading: "", result: "空"),
            LearningEntry(reading: "から", result: ""),
        ])
        XCTAssertEqual(store.current.entries.count, 1)
        XCTAssertEqual(store.current.entries.first?.result, "貴社")
    }

    /// 全部消すと空になる
    func testReplaceAllWithEmptyClears() throws {
        let store = LearningStore(url: url)
        store.replaceAll([LearningEntry(reading: "あ", result: "亜")])
        XCTAssertEqual(store.count, 1)
        store.replaceAll([])
        XCTAssertEqual(store.count, 0)
        XCTAssertTrue(store.current.isEmpty)
    }
}
