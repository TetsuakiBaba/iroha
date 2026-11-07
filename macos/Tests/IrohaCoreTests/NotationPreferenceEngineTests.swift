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

final class DocumentTopicTests: XCTestCase {

    private func sentence(_ words: [(String, String)]) -> [ReadingToken] {
        words.map { ReadingToken(surface: $0.0, reading: $0.1) }
    }

    func testWordPenaltyGroupsVariantsAsOneWord() {
        let preference = NotationPreference(counts: ["ふとう": ["符頭": 14], "あげ": ["挙げ": 3, "あげ": 2, "上げ": 1]])
        XCTAssertEqual(preference.wordPenalty(surface: "符頭", reading: "ふとう"), 0)
        XCTAssertEqual(preference.wordPenalty(surface: "不等", reading: "ふとう"), log(1.0 / 15.0), accuracy: 1e-9)
        // 挙げ＋あげ（5 回）と 上げ＋あげ（3 回）を語として比べる
        XCTAssertEqual(preference.wordPenalty(surface: "上げ", reading: "あげ"), log(4.0 / 6.0), accuracy: 1e-9)
        XCTAssertEqual(preference.wordAlternatives(for: "不等", reading: "ふとう"), ["符頭"])
        // 表記の違いは別の語として試さない
        XCTAssertEqual(preference.wordAlternatives(for: "あげ", reading: "あげ"), [])
    }

    func testTopicIndexIgnoresWordsInEveryDocument() {
        let music = [sentence([("研究", "けんきゅう"), ("記譜", "きふ"), ("演奏", "えんそう")])]
        let vision = [sentence([("研究", "けんきゅう"), ("白杖", "はくじょう"), ("歩行", "ほこう")])]
        let index = DocumentTopicIndex(documents: [music, vision])
        XCTAssertEqual(index.nearest(sentence([("演奏", "えんそう")]))?.index, 0)
        XCTAssertEqual(index.nearest(sentence([("歩行", "ほこう")]))?.index, 1)
        // どの文書にも出る語・文書に無い語だけなら話題は決まらない
        XCTAssertNil(index.nearest(sentence([("研究", "けんきゅう"), ("掃除機", "そうじき")])))
    }

    func testSharedTokensExcludeTheChoice() {
        let shared = NotationPreferenceEngine<FakeTopicScorer>.sharedTokens([
            sentence([("不等", "ふとう"), ("の", "の"), ("演奏", "えんそう")]),
            sentence([("符頭", "ふとう"), ("の", "の"), ("演奏", "えんそう")]),
        ])
        XCTAssertEqual(shared.map(\.surface), ["の", "演奏"])
    }

    func testDifferentWordIsChosenOnlyWhenTopicIsNear() async throws {
        let tokenizer = TopicTokenizer()
        let music = [sentence([("符頭", "ふとう"), ("演奏", "えんそう"), ("楽譜", "がくふ")])]
        let other = [sentence([("白杖", "はくじょう"), ("歩行", "ほこう")])]
        let preferences = [
            NotationPreference(counts: ["ふとう": ["符頭": 14]]), NotationPreference(counts: [:]),
        ]
        let configuration = NotationPreferenceConfiguration(
            preference: .empty, weight: 0, documentPreferences: preferences,
            topicIndex: DocumentTopicIndex(documents: [music, other]), topicWeight: 4)
        let scores: [String: Float] = ["不等の演奏": -1.0, "符頭の演奏": -2.0, "不等の歩行": -1.0, "符頭の歩行": -2.0]
        // 話題が音楽の文書に近い（演奏）→ 符頭
        let near = NotationPreferenceEngine(base: FakeTopicScorer(generated: "不等の演奏", scores: scores),
                                            tokenizer: tokenizer) { configuration }
        let nearResult = try await near.convert(reading: "ふとうのえんそう", context: "", candidateCount: 1)
        XCTAssertEqual(nearResult, ["符頭の演奏"])
        // 話題が別の文書（歩行）→ 試さない
        let far = NotationPreferenceEngine(base: FakeTopicScorer(generated: "不等の歩行", scores: scores),
                                           tokenizer: tokenizer) { configuration }
        let farResult = try await far.convert(reading: "ふとうのほこう", context: "", candidateCount: 1)
        XCTAssertEqual(farResult, ["不等の歩行"])
    }
}

private actor FakeTopicScorer: ConversionEngine, CandidateScorer {
    let generated: String
    let scores: [String: Float]

    init(generated: String, scores: [String: Float]) {
        self.generated = generated
        self.scores = scores
    }

    func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] { [generated] }

    func score(candidates: [String], reading: String, context: String) async throws -> [Float] {
        candidates.map { scores[$0] ?? -100 }
    }
}

/// 「符頭／不等」「の」「演奏」「歩行」だけを知っている偽の分かち書き
private struct TopicTokenizer: ReadingTokenizer {
    func tokens(in text: String) -> [ReadingToken] {
        let words = ["符頭": "ふとう", "不等": "ふとう", "の": "の", "演奏": "えんそう", "歩行": "ほこう"]
        var result: [ReadingToken] = []
        var rest = Substring(text)
        while !rest.isEmpty {
            if let (word, reading) = words.first(where: { rest.hasPrefix($0.key) }) {
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
