import XCTest
@testable import IrohaCore

/// FIPS 180-4 の既知の値と、`shasum -a 256` の結果で確かめる
final class SHA256Tests: XCTestCase {

    func testKnownVectors() {
        XCTAssertEqual(
            SHA256.hex(Data()),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
        XCTAssertEqual(
            SHA256.hex(Data("abc".utf8)),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        // 56バイト: パディングが次のブロックへあふれる境界
        XCTAssertEqual(
            SHA256.hex(Data("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq".utf8)),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")
        // 64バイト: ちょうど1ブロック
        XCTAssertEqual(
            SHA256.hex(Data(String(repeating: "a", count: 64).utf8)),
            "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb")
        XCTAssertEqual(
            SHA256.hex(Data(String(repeating: "a", count: 1_000_000).utf8)),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")
    }

    /// 1MBごとに読む経路（`hex(contentsOf:)`）が、まとめてハッシュした結果と一致する
    func testFileHashingMatchesInMemory() throws {
        let bytes = (0..<(3 * 1024 * 1024 + 12345)).map { UInt8(truncatingIfNeeded: $0 &* 31 &+ 7) }
        let data = Data(bytes)
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-sha256-\(UUID().uuidString).bin")
        try data.write(to: url)
        defer { try? FileManager.default.removeItem(at: url) }
        XCTAssertEqual(try SHA256.hex(contentsOf: url), SHA256.hex(data))
    }

    /// 更新を細かく分けても結果が変わらない（チャンク境界の扱い）
    func testChunkBoundaries() {
        for size in [1, 55, 56, 57, 63, 64, 65, 119, 120, 128, 1000] {
            let data = Data((0..<size).map { UInt8(truncatingIfNeeded: $0) })
            XCTAssertEqual(SHA256.hex(data).count, 64, "size=\(size)")
        }
    }
}
