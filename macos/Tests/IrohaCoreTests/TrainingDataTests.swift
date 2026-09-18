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

    /// 正解 `correct` 件 → 間違い `mistakes` 件の順に並んだ記録と、その変換し直し結果
    private func records(mistakes: Int, correct: Int) -> (entries: [ConversionLogEntry], screening: TrainingScreener.Screening) {
        var screening = TrainingScreener.Screening()
        screening.correct = (0..<correct).map { unchanged("せいかい\($0)", "正解\($0)", at: TimeInterval($0)) }
        screening.mistakes = (0..<mistakes).map {
            correction("まちがい\($0)", proposed: "間違\($0)", committed: "真違\($0)", at: TimeInterval(1000 + $0))
        }
        return (screening.correct + screening.mistakes, screening)
    }

    /// 評価用は時系列の末尾から取り、残り全件が訓練データになる（重み付けや混合はしない）
    func testStratifyHoldsOutTailAndTrainsOnTheRest() {
        let (entries, screening) = records(mistakes: 12, correct: 100)
        let split = TrainingDataBuilder.stratify(entries: entries, screening: screening)

        // 間違い 12 件のうち 3 分の 1 = 4 件を評価に回す（新しい方から）
        XCTAssertEqual(split.heldOutMistakes.map(\.reading), ["まちがい8", "まちがい9", "まちがい10", "まちがい11"])
        // 正解できた記録は上限 40 件まで評価（壊れていないかの確認）に回す
        XCTAssertEqual(split.heldOutCorrect.count, 40)
        XCTAssertEqual(split.heldOutCorrect.first?.reading, "せいかい60")
        // 訓練は残り全件・時系列順・重複なし
        XCTAssertEqual(split.train.count, 112 - 4 - 40)
        XCTAssertEqual(split.trainLines.count, split.train.count)
        XCTAssertEqual(split.train.first?.reading, "せいかい0")
        XCTAssertEqual(split.train.last?.reading, "まちがい7")
        XCTAssertTrue(split.train.allSatisfy { !split.heldOutMistakes.contains($0) && !split.heldOutCorrect.contains($0) })
    }

    /// 変換し直しの上限に入らなかった古い記録も訓練には使う
    func testStratifyTrainsOnUnscreenedEntriesToo() {
        let (entries, screening) = records(mistakes: 6, correct: 10)
        let old = [unchanged("ふるい", "古い", at: -100)]
        let split = TrainingDataBuilder.stratify(entries: old + entries, screening: screening)
        XCTAssertEqual(split.train.first?.reading, "ふるい")
        XCTAssertEqual(split.train.count, 1 + 16 - 2 - 5)
    }

    /// 間違いが少ないときも 1 件は評価に取り分ける（0 だと効果が測れない）
    func testStratifyWithFewMistakes() {
        let (entries, screening) = records(mistakes: 3, correct: 20)
        let split = TrainingDataBuilder.stratify(entries: entries, screening: screening)
        XCTAssertEqual(split.heldOutMistakes.count, 1)

        // 間違いが 1 件だけなら評価には回さず学習に使う（唯一の例を評価に取られると学ぶものが無くなる）
        let (single, singleScreening) = records(mistakes: 1, correct: 20)
        let singleSplit = TrainingDataBuilder.stratify(entries: single, screening: singleScreening)
        XCTAssertTrue(singleSplit.heldOutMistakes.isEmpty)
        XCTAssertTrue(singleSplit.train.contains { $0.reading == "まちがい0" })

        // 間違いが 0 件でも学習はできる（効果は測れないが、その人の文章は学べる）
        let (none, noneScreening) = records(mistakes: 0, correct: 20)
        let noneSplit = TrainingDataBuilder.stratify(entries: none, screening: noneScreening)
        XCTAssertTrue(noneSplit.heldOutMistakes.isEmpty)
        XCTAssertEqual(noneSplit.train.count, 10)
    }

    /// 評価用の上限（間違い 25 件・正解 40 件）
    func testStratifyCapsHeldOut() {
        let (entries, screening) = records(mistakes: 300, correct: 1000)
        let split = TrainingDataBuilder.stratify(entries: entries, screening: screening)
        XCTAssertEqual(split.heldOutMistakes.count, TrainingDataBuilder.maxHeldOutMistakes)
        XCTAssertEqual(split.heldOutCorrect.count, TrainingDataBuilder.maxHeldOutCorrect)
        XCTAssertEqual(split.train.count, 1300 - 25 - 40)
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

    /// JSON Lines の往復（ヘルパー → 設定画面）
    func testTrainingEventRoundTrip() throws {
        let events: [TrainingEvent] = [
            .data(TrainingDataSummary(records: 400, screened: 385, mistakes: 12, trainLines: 356, heldOutMistakes: 4,
                                      heldOutCorrect: 40)),
            .stage("train"),
            .progress(stage: "screen", done: 120, total: 385),
            .step(TrainingStep(epoch: 1, epochs: 3, step: 4, steps: 24, loss: 1.25, elapsed: 2.5)),
            .eval(phase: "before", scores: TrainingScores(mistakes: TrainingScore(exact: 0, total: 2),
                                                          correct: TrainingScore(exact: 40, total: 40))),
            .done(TrainingResult(adapter: "/tmp/a.gguf", mistakesTSV: "/tmp/a.mistakes.tsv", correctTSV: nil,
                                 trainTSV: nil, before: TrainingScores(), after: TrainingScores(),
                                 data: TrainingDataSummary(records: 1, screened: 1, mistakes: 1, trainLines: 1,
                                                           heldOutMistakes: 0, heldOutCorrect: 0),
                                 elapsed: 42.5)),
            .error("失敗"),
        ]
        for event in events {
            XCTAssertEqual(TrainingEvent.parse(line: event.jsonLine()), event)
        }
        XCTAssertNil(TrainingEvent.parse(line: "{}"))
    }
}
