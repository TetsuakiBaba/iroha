import XCTest
@testable import IrohaCore

/// 採点と生成を差し替えられる偽のzenz
private actor FakeBase: ConversionEngine, CandidateScorer {
    var generated: [String]
    var scores: [String: Float]
    private(set) var scoreCalls = 0
    private(set) var convertCalls: [Int] = []

    init(generated: [String], scores: [String: Float]) {
        self.generated = generated
        self.scores = scores
    }

    func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        convertCalls.append(candidateCount)
        return Array(generated.prefix(max(candidateCount, 1)))
    }

    func score(candidates: [String], reading: String, context: String) async throws -> [Float] {
        scoreCalls += 1
        return candidates.map { scores[$0] ?? -100 }
    }
}

final class LatticeRescoringEngineTests: XCTestCase {

    private static let dictionaryURL = LatticeConverter.defaultDictionaryURL()

    private func makeLattice() throws -> LatticeConverter {
        guard let url = Self.dictionaryURL else {
            throw XCTSkip("辞書が無いためスキップ（macos/scripts/fetch-dictionary.sh で取得）")
        }
        return LatticeConverter(dictionaryURL: url)
    }

    /// ラティスの候補は読み全体に一致し、読みの合わない語（活用を・NIPPON）を含まない
    func testLatticeCandidatesAllMatchReading() async throws {
        let lattice = try makeLattice()
        let candidates = await lattice.candidates(reading: "ないようを", count: 10)
        XCTAssertFalse(candidates.isEmpty)
        XCTAssertTrue(candidates.contains("内容を"))
        XCTAssertFalse(candidates.contains("活用を"))
        XCTAssertFalse(candidates.contains("NIPPON"))
        for candidate in candidates {
            XCTAssertTrue(candidate.hasSuffix("を") || candidate.hasSuffix("ヲ"), candidate)
        }
        // 部分候補（「内容」「ない」など）は含まれない
        XCTAssertFalse(candidates.contains("内容"))
        XCTAssertFalse(candidates.contains("ない"))
    }

    /// 第一候補（ライブ変換）はラティスを通さずzenzの生成に任せる
    func testSingleCandidateBypassesLattice() async throws {
        let lattice = try makeLattice()
        let base = FakeBase(generated: ["記者"], scores: [:])
        let engine = LatticeRescoringEngine(base: base, lattice: lattice)
        let result = try await engine.convert(reading: "きしゃ", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["記者"])
        let calls = await base.scoreCalls
        XCTAssertEqual(calls, 0)
    }

    /// 候補ウィンドウはラティス候補＋zenzの生成をzenzの採点順に並べる
    func testMultipleCandidatesAreLatticeCandidatesRankedByScore() async throws {
        let lattice = try makeLattice()
        // zenzの答えは辞書にない語（採点は最上位）。辞書候補は 貴社 > 記者 > 汽車 の順に採点
        let base = FakeBase(
            generated: ["キシャ社"],
            scores: ["キシャ社": -0.5, "貴社": -1.0, "記者": -2.0, "汽車": -3.0])
        let engine = LatticeRescoringEngine(base: base, lattice: lattice)
        let result = try await engine.convert(reading: "きしゃ", context: "", candidateCount: 4)
        XCTAssertEqual(result, ["キシャ社", "貴社", "記者", "汽車"])
        let calls = await base.scoreCalls
        XCTAssertEqual(calls, 1)
    }

    /// 辞書が候補を作れない読みはzenzのn-bestにそのまま任せる
    func testFallsBackToBaseWhenLatticeHasNoCandidates() async throws {
        let lattice = try makeLattice()
        let base = FakeBase(generated: ["Ａ", "Ｂ"], scores: [:])
        let engine = LatticeRescoringEngine(base: base, lattice: lattice)
        // 空の読みはラティスが候補を返さない
        let result = try await engine.convert(reading: "", context: "", candidateCount: 2)
        XCTAssertEqual(result, ["Ａ", "Ｂ"])
        let calls = await base.scoreCalls
        XCTAssertEqual(calls, 0)
    }
}
