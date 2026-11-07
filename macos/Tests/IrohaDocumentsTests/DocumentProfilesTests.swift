import XCTest
@testable import IrohaDocuments

final class DocumentProfilesTests: XCTestCase {

    private var root: URL!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-profiles-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: root)
    }

    private func write(_ path: String, _ text: String = "本稿では記譜を扱う。") throws {
        let url = root.appendingPathComponent(path)
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try text.write(to: url, atomically: true, encoding: .utf8)
    }

    func testEmptyFolderHasNoProfiles() {
        let result = DocumentProfiles.folders(in: root)
        XCTAssertEqual(result.folders, [])
        XCTAssertEqual(result.ignoredFiles, [])
    }

    func testTopLevelFilesFormOneProfileWithoutSubfolders() throws {
        try write("a.txt")
        try write("b.md")
        let result = DocumentProfiles.folders(in: root)
        XCTAssertEqual(result.folders.map(\.name), [""])
        XCTAssertEqual(result.ignoredFiles, [])
    }

    func testSubfoldersBecomeProfilesAndTopLevelFilesAreIgnored() throws {
        try write("a.txt")
        try write("論文/2024/p1.txt")
        try write("太宰治/人間失格.txt")
        try write("空/memo.png")  // 対応する文書が無いフォルダは数えない
        let result = DocumentProfiles.folders(in: root)
        XCTAssertEqual(result.folders.map(\.name).sorted(), ["太宰治", "論文"])
        XCTAssertEqual(result.ignoredFiles, ["a.txt"])
        let paper = try XCTUnwrap(result.folders.first { $0.name == "論文" })
        XCTAssertEqual(DocumentCollection.documentURLs(in: paper.url).map(\.lastPathComponent), ["p1.txt"])
    }

    func testResolveSelection() {
        let names = ["論文", "太宰治"]
        XCTAssertEqual(DocumentProfileSelection.resolve(stored: nil, names: names), "論文")
        XCTAssertEqual(DocumentProfileSelection.resolve(stored: "太宰治", names: names), "太宰治")
        XCTAssertEqual(DocumentProfileSelection.resolve(stored: "消したフォルダ", names: names), "論文")
        XCTAssertNil(DocumentProfileSelection.resolve(stored: DocumentProfileSelection.off, names: names))
        XCTAssertNil(DocumentProfileSelection.resolve(stored: nil, names: []))
        XCTAssertEqual(DocumentProfileSelection.resolve(stored: nil, names: [""]), "")
    }

    func testNextCyclesThroughOff() {
        let names = ["論文", "太宰治"]
        let off = DocumentProfileSelection.off
        XCTAssertEqual(DocumentProfileSelection.next(stored: off, names: names), "論文")
        XCTAssertEqual(DocumentProfileSelection.next(stored: "論文", names: names), "太宰治")
        XCTAssertEqual(DocumentProfileSelection.next(stored: "太宰治", names: names), off)
        XCTAssertEqual(DocumentProfileSelection.next(stored: nil, names: names), "太宰治")
        XCTAssertEqual(DocumentProfileSelection.next(stored: "", names: [""]), off)
        XCTAssertEqual(DocumentProfileSelection.next(stored: off, names: [""]), "")
        XCTAssertEqual(DocumentProfileSelection.next(stored: off, names: []), off)
    }
}
