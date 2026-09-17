import XCTest
@testable import IrohaCore

final class TrainingDataTests: XCTestCase {

    private func entry(_ reading: String, _ committed: String, context: String = "", proposed: String? = nil,
                       at seconds: TimeInterval = 0) -> ConversionLogEntry {
        ConversionLogEntry(timestamp: Date(timeIntervalSince1970: 1_700_000_000 + seconds), mode: .live,
                           context: context, contextSource: context.isEmpty ? .none : .document,
                           reading: reading, proposed: proposed, committed: committed, model: "test")
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

    func testSplitTakesTailAsHeldOut() {
        let entries = (0..<100).map { entry("よみ\($0)", "読み\($0)", at: TimeInterval($0)) }
        let (train, heldOut) = TrainingDataBuilder.split(entries, heldOutFraction: 0.1, minHeldOut: 20)
        XCTAssertEqual(heldOut.count, 20)  // 10% = 10 だが最低 20
        XCTAssertEqual(train.count, 80)
        XCTAssertEqual(heldOut.first?.reading, "よみ80")

        let few = Array(entries.prefix(10))
        let (fewTrain, fewHeldOut) = TrainingDataBuilder.split(few, heldOutFraction: 0.1, minHeldOut: 20)
        XCTAssertEqual(fewHeldOut.count, 5)  // 半分まで
        XCTAssertEqual(fewTrain.count, 5)

        let (oneTrain, oneHeldOut) = TrainingDataBuilder.split([entries[0]], heldOutFraction: 0.1, minHeldOut: 20)
        XCTAssertEqual(oneTrain.count, 1)
        XCTAssertTrue(oneHeldOut.isEmpty)
    }

    func testTrainingLinesDuplicateEdited() {
        let entries = [entry("きしゃ", "貴社", proposed: "記者"), entry("きしゃ", "記者", proposed: "記者")]
        XCTAssertEqual(TrainingDataBuilder.trainingLines(entries, duplicateEdited: true).count, 3)
        XCTAssertEqual(TrainingDataBuilder.trainingLines(entries, duplicateEdited: false).count, 2)
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

    func testRecommendedConfigRaisesEpochsForSmallData() {
        XCTAssertEqual(TrainingConfig.recommended(forExampleCount: 100).epochs, 5)
        XCTAssertEqual(TrainingConfig.recommended(forExampleCount: 1000).epochs, 3)
    }
}
