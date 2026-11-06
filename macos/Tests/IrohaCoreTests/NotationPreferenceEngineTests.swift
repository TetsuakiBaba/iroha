import XCTest
@testable import IrohaCore

/// 生成と採点を差し替えられる偽のモデル
private actor FakeScorer: ConversionEngine, CandidateScorer {
    let generated: String
    let scores: [String: Float]
    private(set) var scoredCandidates: [[String]] = []

    init(generated: String, scores: [String: Float]) {
        self.generated = generated
        self.scores = scores
    }

    func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        [generated]
    }

    func score(candidates: [String], reading: String, context: String) async throws -> [Float] {
        scoredCandidates.append(candidates)
        return candidates.map { scores[$0] ?? -100 }
    }
}

/// 決まった語の区切りと読みを返す偽の分かち書き（表に無い文字は 1 文字ずつ読みなし）
private struct FakeTokenizer: ReadingTokenizer {
    let words: [String: String]

    func tokens(in text: String) -> [ReadingToken] {
        var result: [ReadingToken] = []
        var rest = Substring(text)
        while !rest.isEmpty {
            if let (word, reading) = words.filter({ rest.hasPrefix($0.key) }).max(by: { $0.key.count < $1.key.count }) {
                result.append(ReadingToken(surface: word, reading: reading))
                rest = rest.dropFirst(word.count)
            } else {
                result.append(ReadingToken(surface: String(rest.first!), reading: nil))
                rest = rest.dropFirst()
            }
        }
        return result
    }
}

final class NotationPreferenceEngineTests: XCTestCase {

    private let tokenizer = FakeTokenizer(words: [
        "及び": "および", "および": "および", "精度": "せいど", "制度": "せいど",
    ])

    private let preference = NotationPreference(counts: [
        "および": ["及び": 9, "および": 3],
        "せいど": ["精度": 20],
    ])

    func testPenaltyIsZeroForTheMostUsedSurface() {
        XCTAssertEqual(preference.penalty(surface: "及び", reading: "および"), 0)
        XCTAssertEqual(preference.penalty(surface: "および", reading: "および"), log(4.0 / 10.0), accuracy: 1e-9)
        // 同じ読みの別の語（表記の違いではない）は不利にしない
        XCTAssertEqual(preference.penalty(surface: "制度", reading: "せいど"), 0)
        // 文書に無い読みは 0
        XCTAssertEqual(preference.penalty(surface: "研究", reading: "けんきゅう"), 0)
    }

    func testOneMoraReadingsAreIgnored() {
        let counts = NotationPreference(counts: ["しゃ": ["者": 50, "車": 1], "み": ["見": 10]])
        XCTAssertEqual(counts.penalty(surface: "車", reading: "しゃ"), 0)
        XCTAssertTrue(counts.alternatives(for: "車", reading: "しゃ").isEmpty)
        XCTAssertNil(counts.preferredSurface(for: "み", reading: "み"))
        XCTAssertEqual(NotationPreference.moraCount("しゃしん"), 3)
    }

    func testAlternativesAreVariantsUsedTwice() {
        let counts = NotationPreference(counts: [
            "きふ": ["記譜": 5, "寄付": 1], "すべて": ["すべて": 10, "全て": 1], "あわせ": ["あわせ": 1, "合わせ": 6],
        ])
        // 別の語には替えない
        XCTAssertEqual(counts.alternatives(for: "寄付", reading: "きふ"), [])
        XCTAssertEqual(counts.alternatives(for: "全て", reading: "すべて"), ["すべて"])
        // 1 回しか使っていない表記は試さない
        XCTAssertEqual(counts.alternatives(for: "合わせ", reading: "あわせ"), [])
    }

    func testFirstCandidateIsRescoredWithPreference() async throws {
        let base = FakeScorer(generated: "AおよびB", scores: ["AおよびB": -1.0, "A及びB": -1.5])
        let engine = NotationPreferenceEngine(base: base, tokenizer: tokenizer) { [preference] in
            NotationPreferenceConfiguration(preference: preference, weight: 1)
        }
        // -1.0 + log(0.4) ≈ -1.92 < -1.5 なので「及び」を選ぶ
        let result = try await engine.convert(reading: "aおよびb", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["A及びB"])
    }

    func testModelStillWinsWhenItIsConfident() async throws {
        let base = FakeScorer(generated: "AおよびB", scores: ["AおよびB": -1.0, "A及びB": -5.0])
        let engine = NotationPreferenceEngine(base: base, tokenizer: tokenizer) { [preference] in
            NotationPreferenceConfiguration(preference: preference, weight: 1)
        }
        let result = try await engine.convert(reading: "aおよびb", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["AおよびB"])
    }

    func testZeroWeightPassesThroughWithoutScoring() async throws {
        let base = FakeScorer(generated: "AおよびB", scores: [:])
        let engine = NotationPreferenceEngine(base: base, tokenizer: tokenizer) { [preference] in
            NotationPreferenceConfiguration(preference: preference, weight: 0)
        }
        let result = try await engine.convert(reading: "aおよびb", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["AおよびB"])
        let calls = await base.scoredCandidates
        XCTAssertTrue(calls.isEmpty)
    }

    func testScoreAddsPenaltyForCandidateWindow() async throws {
        let base = FakeScorer(generated: "", scores: ["AおよびB": -1.0, "A及びB": -2.0, "制度": -1.0, "精度": -2.0])
        let engine = NotationPreferenceEngine(base: base, tokenizer: tokenizer) { [preference] in
            NotationPreferenceConfiguration(preference: preference, weight: 1)
        }
        let scores = try await engine.score(candidates: ["AおよびB", "A及びB"], reading: "aおよびb", context: "")
        XCTAssertEqual(Double(scores[0]), -1.0 + log(4.0 / 10.0), accuracy: 1e-5)
        XCTAssertEqual(scores[1], -2.0)
        // 別の語の組は並びを変えない
        let words = try await engine.score(candidates: ["制度", "精度"], reading: "せいど", context: "")
        XCTAssertEqual(words, [-1.0, -2.0])
    }

    func testRewriteCandidatesIncludeAllFavoritesFirst() {
        let tokenizer = FakeTokenizer(words: [
            "及び": "および", "および": "および", "全て": "すべて", "すべて": "すべて", "制度": "せいど",
        ])
        let preference = NotationPreference(counts: [
            "および": ["及び": 9, "および": 3], "すべて": ["すべて": 5, "全て": 2], "せいど": ["精度": 20],
        ])
        let configuration = NotationPreferenceConfiguration(preference: preference, weight: 1)
        let engine = NotationPreferenceEngine(base: FakeScorer(generated: "", scores: [:]), tokenizer: tokenizer) {
            configuration
        }
        let candidates = engine.rewriteCandidates(of: "全ておよび制度", configuration: configuration)
        XCTAssertEqual(candidates.first, "全ておよび制度")
        XCTAssertEqual(candidates[1], "すべて及び制度")
        XCTAssertEqual(Set(candidates), ["全ておよび制度", "すべて及び制度", "すべておよび制度", "全て及び制度"])
    }
}

final class NotationPreferenceContextTests: XCTestCase {

    private func tokens(_ pairs: [(String, String?)]) -> [ReadingToken] {
        pairs.map { ReadingToken(surface: $0.0, reading: $0.1) }
    }

    func testKanaInFixedExpressionsIsNotCounted() {
        // 「研究において」の「おい」は数えず、「主眼をおいて」の「おい」は数える
        let preference = NotationPreference(sentences: [
            tokens([("研究", "けんきゅう"), ("に", "に"), ("おい", "おい"), ("て", "て")]),
            tokens([("主眼", "しゅがん"), ("を", "を"), ("おい", "おい"), ("て", "て")]),
        ])
        XCTAssertEqual(preference.counts["おい"], ["おい": 1])
    }

    func testOnlyWhenTheAuthorUsesSeveralVariants() {
        let onlyKana = NotationPreference(counts: ["あっ": ["あっ": 40]])
        XCTAssertEqual(onlyKana.penalty(surface: "会っ", reading: "あっ"), 0)
        XCTAssertTrue(onlyKana.alternatives(for: "会っ", reading: "あっ").isEmpty)
        let both = NotationPreference(counts: ["すべて": ["すべて": 10, "全て": 3]])
        XCTAssertLessThan(both.penalty(surface: "全て", reading: "すべて"), 0)
    }

    func testFixedPositionsAreNotScored() {
        let preference = NotationPreference(counts: ["おい": ["置い": 5, "おい": 1]])
        // 「どこにおいて」の「おい」は「に」の直後なので不利にしない
        let sentence = tokens([("どこ", "どこ"), ("に", "に"), ("おい", "おい"), ("て", "て")])
        XCTAssertEqual(preference.penalty(sentence), 0)
        // 漢字でも同じ位置なら当てない（「どこに行って」を「ていく」のかなと比べない）
        let kanji = NotationPreference(counts: ["いっ": ["いっ": 30, "行っ": 2]])
        XCTAssertEqual(kanji.penalty(tokens([("どこ", "どこ"), ("に", "に"), ("行っ", "いっ"), ("て", "て")])), 0)
        // 引用の「といった」のかなは数えない
        let counted = NotationPreference(sentences: [tokens([("例", "れい"), ("と", "と"), ("いっ", "いっ"), ("た", "た")])])
        XCTAssertNil(counted.counts["いっ"])
        XCTAssertLessThan(preference.penalty(tokens([("おい", "おい"), ("て", "て")])), 0)
    }
}
