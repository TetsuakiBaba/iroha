import XCTest
@testable import IrohaCore

final class TrainingDataTests: XCTestCase {

    private func entry(_ reading: String, _ committed: String, context: String = "", proposed: String? = nil,
                       at seconds: TimeInterval = 0) -> ConversionLogEntry {
        ConversionLogEntry(timestamp: Date(timeIntervalSince1970: 1_700_000_000 + seconds), mode: .live,
                           context: context, contextSource: context.isEmpty ? .none : .document,
                           reading: reading, proposed: proposed, committed: committed, model: "test")
    }

    /// モデルの出力をそのまま確定した記録（proposed == committed）
    private func unchanged(_ reading: String, _ committed: String, at seconds: TimeInterval = 0) -> ConversionLogEntry {
        entry(reading, committed, proposed: committed, at: seconds)
    }

    /// モデルの出力を直した記録
    private func correction(_ reading: String, proposed: String, committed: String,
                            at seconds: TimeInterval = 0) -> ConversionLogEntry {
        entry(reading, committed, proposed: proposed, at: seconds)
    }

    /// 文字単位の偽トークナイザ（U+EE0x のタグも 1 文字 = 1 トークン）
    private func charTokenize(_ text: String) -> [Int32] {
        text.unicodeScalars.map { Int32($0.value) }
    }

    func testUsableFiltersEmptyAndNonKana() {
        XCTAssertTrue(TrainingDataBuilder.isUsable(entry("きしゃ", "記者")))
        XCTAssertFalse(TrainingDataBuilder.isUsable(entry("きしゃ", "  ")))
        XCTAssertFalse(TrainingDataBuilder.isUsable(entry("abc", "ABC")))
        XCTAssertFalse(TrainingDataBuilder.isUsable(entry("きしゃ", "記\n者")))
    }

    func testDedupeKeepsLastOccurrenceInOrder() {
        let entries = [entry("あ", "亜", at: 0), entry("い", "医", at: 1), entry("あ", "亜", at: 2), entry("う", "雨", at: 3)]
        let deduped = TrainingDataBuilder.dedupe(entries)
        XCTAssertEqual(deduped.map(\.reading), ["い", "あ", "う"])
        XCTAssertEqual(deduped[1].timestamp, entries[2].timestamp)
    }

    func testIsCorrection() {
        XCTAssertTrue(TrainingDataBuilder.isCorrection(correction("きしゃ", proposed: "記者", committed: "貴社")))
        XCTAssertFalse(TrainingDataBuilder.isCorrection(unchanged("きしゃ", "記者")))
        // 提示が不明（F6〜F10 など）は、ユーザが形を選んだので修正として扱う
        XCTAssertTrue(TrainingDataBuilder.isCorrection(entry("ない", "ない", proposed: nil)))
    }

    private func screening(mistakes: Int, correct: Int) -> TrainingScreener.Screening {
        var result = TrainingScreener.Screening()
        result.correct = (0..<correct).map { unchanged("せいかい\($0)", "正解\($0)", at: TimeInterval($0)) }
        result.mistakes = (0..<mistakes).map {
            correction("まちがい\($0)", proposed: "間違\($0)", committed: "真違\($0)", at: TimeInterval(1000 + $0))
        }
        return result
    }

    /// モデルが間違えた記録を重み付けし、正解できた記録はアンカーとして混ぜる。評価用は時系列の末尾から取る
    func testStratifyWeightsMistakesAndMixesAnchors() {
        var config = TrainingConfig()
        config.mistakeWeight = 8
        config.anchorRatio = 1.0
        let split = TrainingDataBuilder.stratify(screening(mistakes: 10, correct: 100), config: config)

        // 間違い 10 件のうち 2 割 = 2 件を評価に回す（新しい方から）
        XCTAssertEqual(split.heldOutMistakes.count, 2)
        XCTAssertEqual(split.heldOutMistakes.map(\.reading), ["まちがい8", "まちがい9"])
        XCTAssertEqual(split.trainMistakes.count, 8)
        // 正解できた記録は上限 40 件まで評価（壊れていないかの確認）に回す
        XCTAssertEqual(split.heldOutCorrect.count, 40)
        // アンカーは「間違い 8 件 × 重み 8 = 64 行」に合わせたいが、評価に 40 件回した残りは 60 件なので 60 件
        XCTAssertEqual(split.anchors.count, 60)
        XCTAssertEqual(split.trainLines.count, 8 * 8 + 60)
        // アンカーは評価に回した末尾 40 件を含まない
        XCTAssertTrue(split.anchors.allSatisfy { !split.heldOutCorrect.contains($0) })
        // アンカーは時系列全体から取る（末尾だけに偏らない）
        XCTAssertEqual(split.anchors.first?.reading, "せいかい0")
    }

    /// 間違いが少ないときも 1 件は評価に取り分ける（0 だと効果が測れない）
    func testStratifyWithFewMistakes() {
        let split = TrainingDataBuilder.stratify(screening(mistakes: 3, correct: 20), config: TrainingConfig())
        XCTAssertEqual(split.heldOutMistakes.count, 1)
        XCTAssertEqual(split.trainMistakes.count, 2)

        // 間違いが 1 件だけなら評価には回さず学習に使う（唯一の例を評価に取られると学ぶものが無くなる）
        let single = TrainingDataBuilder.stratify(screening(mistakes: 1, correct: 20), config: TrainingConfig())
        XCTAssertTrue(single.heldOutMistakes.isEmpty)
        XCTAssertEqual(single.trainMistakes.count, 1)
    }

    /// アンカーを 0 にすると間違いだけで学習する
    func testStratifyWithoutAnchors() {
        var config = TrainingConfig()
        config.anchorRatio = 0
        config.mistakeWeight = 4
        let split = TrainingDataBuilder.stratify(screening(mistakes: 10, correct: 50), config: config)
        XCTAssertTrue(split.anchors.isEmpty)
        XCTAssertEqual(split.trainLines.count, split.trainMistakes.count * 4)
    }

    func testSampleSpreadsEvenly() {
        XCTAssertEqual(TrainingDataBuilder.sample(Array(0..<10), count: 5), [0, 2, 4, 6, 8])
        XCTAssertEqual(TrainingDataBuilder.sample(Array(0..<3), count: 10), [0, 1, 2])
        XCTAssertTrue(TrainingDataBuilder.sample(Array(0..<10), count: 0).isEmpty)
    }

    func testEncodeSetsLossFromAtOutputTag() throws {
        let line = entry("きしゃ", "記者", context: "本日は").trainingLine
        let examples = try TrainingDataBuilder.encode(lines: [line], tokenize: charTokenize, eos: 2, outputTag: [0xEE01])
        XCTAssertEqual(examples.count, 1)
        let example = examples[0]
        // "\u{EE02}本日は\u{EE00}キシャ\u{EE01}記者" + EOS
        XCTAssertEqual(example.tokens.last, 2)
        XCTAssertEqual(example.tokens[example.lossFrom], 0xEE01)
        // 損失は targets[t] = tokens[t+1] が「記」から EOS まで
        XCTAssertEqual(example.tokens[example.lossFrom + 1], Int32(("記" as Unicode.Scalar).value))
        XCTAssertEqual(example.tokens.count - 1 - example.lossFrom, 3)  // 記・者・EOS
    }

    func testEncodeRejectsMissingTagAndSkipsEmptyOutput() {
        XCTAssertThrowsError(try TrainingDataBuilder.encode(lines: ["タグなし"], tokenize: charTokenize, eos: 2, outputTag: [0xEE01]))
        // 出力が空の行だけなら例が無い
        XCTAssertThrowsError(try TrainingDataBuilder.encode(lines: ["\u{EE00}ア\u{EE01}"], tokenize: charTokenize, eos: 2, outputTag: [0xEE01]))
    }

    /// タグが複数トークン（zenz のバイトフォールバック）でも末尾で損失位置が決まる
    func testEncodeWithMultiTokenTag() throws {
        // 偽トークナイザ: U+EE01 を [9, 8, 7] の 3 トークンにする
        func tokenize(_ text: String) -> [Int32] {
            text.unicodeScalars.flatMap { $0.value == 0xEE01 ? [9, 8, 7] : [Int32($0.value)] }
        }
        let examples = try TrainingDataBuilder.encode(lines: ["\u{EE00}ア\u{EE01}亜"], tokenize: tokenize, eos: 2, outputTag: [9, 8, 7])
        XCTAssertEqual(examples[0].tokens, [0xEE00, Int32(("ア" as Unicode.Scalar).value), 9, 8, 7, Int32(("亜" as Unicode.Scalar).value), 2])
        XCTAssertEqual(examples[0].lossFrom, 4)
        XCTAssertNil(TrainingDataBuilder.lastIndexOfSubsequence([9, 8, 7], in: [9, 8]))
        XCTAssertEqual(TrainingDataBuilder.lastIndexOfSubsequence([1, 2], in: [1, 2, 1, 2, 3]), 3)
    }

    func testHeldOutTSVEscapesTabsAndNewlines() {
        let tsv = TrainingDataBuilder.heldOutTSV([entry("きしゃ", "記者", context: "本日\tは")])
        XCTAssertEqual(tsv, "きしゃ\t記者\t本日 は\n")
        XCTAssertEqual(TrainingDataBuilder.heldOutTSV([]), "")
    }

    /// 学べる例が少ないうちは弱い設定（悪化を出さない側）、増えたら学習率を上げる
    func testRecommendedConfigIsGentleWhenMistakesAreFew() {
        let few = TrainingConfig.recommended(forMistakeCount: 10)
        XCTAssertEqual(few.learningRate, 5e-5)
        XCTAssertEqual(few.anchorRatio, 2.0)
        let many = TrainingConfig.recommended(forMistakeCount: 200)
        XCTAssertEqual(many.learningRate, 1e-4)
        XCTAssertEqual(many.anchorRatio, 1.0)
    }

    /// JSON Lines の往復（ヘルパー → 設定画面）
    func testTrainingEventRoundTrip() throws {
        let events: [TrainingEvent] = [
            .data(TrainingDataSummary(trainLines: 128, screened: 385, mistakes: 8, anchors: 64, heldOutMistakes: 2,
                                      heldOutCorrect: 40)),
            .stage("train"),
            .progress(stage: "screen", done: 120, total: 385),
            .step(TrainingStep(epoch: 1, epochs: 3, step: 4, steps: 24, loss: 1.25, elapsed: 2.5)),
            .eval(phase: "before", scores: TrainingScores(mistakes: TrainingScore(exact: 0, total: 2),
                                                          correct: TrainingScore(exact: 40, total: 40))),
            .done(TrainingResult(adapter: "/tmp/a.gguf", mistakesTSV: "/tmp/a.mistakes.tsv", correctTSV: nil,
                                 trainTSV: nil, before: TrainingScores(), after: TrainingScores(),
                                 data: TrainingDataSummary(trainLines: 1, screened: 1, mistakes: 1, anchors: 0,
                                                           heldOutMistakes: 0, heldOutCorrect: 0))),
            .error("失敗"),
        ]
        for event in events {
            XCTAssertEqual(TrainingEvent.parse(line: event.jsonLine()), event)
        }
        XCTAssertNil(TrainingEvent.parse(line: "{}"))
    }
}
