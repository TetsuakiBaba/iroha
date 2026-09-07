import Foundation
import XCTest
@testable import IrohaCore

final class DataDirectoryTests: XCTestCase {

    private func makeTempDir() throws -> URL {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-test-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    /// 移行時のコピーは、移行先に無いものだけ運び、既にあるものは上書きしない
    func testCopyPortableItemsSkipsExistingFiles() throws {
        let from = try makeTempDir()
        let to = try makeTempDir()
        defer {
            try? FileManager.default.removeItem(at: from)
            try? FileManager.default.removeItem(at: to)
        }
        try FileManager.default.createDirectory(
            at: from.appendingPathComponent("models"), withIntermediateDirectories: true)
        try "model".write(to: from.appendingPathComponent("models/a.gguf"), atomically: true, encoding: .utf8)
        try "partial".write(to: from.appendingPathComponent("models/a.gguf.tmp"), atomically: true, encoding: .utf8)
        try "from".write(to: from.appendingPathComponent("learning.json"), atomically: true, encoding: .utf8)
        try "from".write(to: from.appendingPathComponent("user-dictionary.json"), atomically: true, encoding: .utf8)
        try "to".write(to: to.appendingPathComponent("user-dictionary.json"), atomically: true, encoding: .utf8)

        let copied = try DataDirectory.copyPortableItems(from: from, to: to)

        XCTAssertEqual(copied, 2)  // models/a.gguf と learning.json
        XCTAssertEqual(
            try String(contentsOf: to.appendingPathComponent("user-dictionary.json"), encoding: .utf8), "to")
        XCTAssertEqual(
            try String(contentsOf: to.appendingPathComponent("models/a.gguf"), encoding: .utf8), "model")
        XCTAssertFalse(FileManager.default.fileExists(atPath: to.appendingPathComponent("models/a.gguf.tmp").path))
    }

    /// ユーザ辞書は外部でファイルが変わったら読み直し、自分の保存では読み直さない
    func testUserDictionaryStoreReloadsOnExternalChange() throws {
        let dir = try makeTempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let url = dir.appendingPathComponent("user-dictionary.json")

        let mine = UserDictionaryStore(url: url)
        mine.add(reading: "いろは", word: "iroha")
        XCTAssertFalse(mine.reloadIfChanged(), "自分の保存は外部変更として扱わない")

        // 別のプロセス（他のMac）が同じファイルに書いたことを模す
        let other = UserDictionaryStore(url: url)
        Thread.sleep(forTimeInterval: 0.02)  // 更新日時が確実に変わるように
        other.add(reading: "にほへ", word: "nihohe")

        XCTAssertEqual(mine.entries.count, 1)
        XCTAssertTrue(mine.reloadIfChanged())
        XCTAssertEqual(mine.entries.map(\.word), ["iroha", "nihohe"])
        XCTAssertFalse(mine.reloadIfChanged())
    }

    /// 学習は外部の変更を置き換えではなくマージで取り込む（両方のMacの学習が残る）
    func testLearningStoreMergesExternalChange() throws {
        let dir = try makeTempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let url = dir.appendingPathComponent("learning.json")

        let mine = LearningStore(url: url)
        mine.record(reading: "きしゃ", result: "記者", segments: [("きしゃ", "記者")])
        Thread.sleep(forTimeInterval: 0.1)  // 非同期保存を待つ

        let other = LearningStore(url: url)
        other.record(reading: "かいしゃ", result: "会社", segments: [("かいしゃ", "会社")])
        Thread.sleep(forTimeInterval: 0.1)

        XCTAssertTrue(mine.reloadIfChanged())
        let readings = Set(mine.current.entries.filter { $0.kind == .sentence }.map(\.reading))
        XCTAssertEqual(readings, ["きしゃ", "かいしゃ"])

        // ファイルが消えた（他のMacでリセット）ならこちらも空にする
        Thread.sleep(forTimeInterval: 0.1)  // マージ結果の書き戻しを待つ
        try FileManager.default.removeItem(at: url)
        XCTAssertTrue(mine.reloadIfChanged())
        XCTAssertEqual(mine.count, 0)
    }

    /// 同じキーは新しい方を採る
    func testLearningMergePrefersNewerEntry() {
        let old = LearningEntry(kind: .sentence, reading: "きしゃ", result: "汽車", updatedAt: Date(timeIntervalSince1970: 1))
        let new = LearningEntry(kind: .sentence, reading: "きしゃ", result: "記者", updatedAt: Date(timeIntervalSince1970: 2))
        XCTAssertEqual(LearningStore.merged(base: [old], recorded: [new]).map(\.result), ["記者"])
        XCTAssertEqual(LearningStore.merged(base: [new], recorded: [old]).map(\.result), ["記者"])
    }
}
