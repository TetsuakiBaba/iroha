import Testing
@testable import IrohaCore

private func scored(_ text: String, margins: [Float]) -> ScoredConversion {
    ScoredConversion(
        text: text, logProb: -1,
        confidences: margins.map { CharacterConfidence(logProb: -0.1, margin: $0, relaxed: false) })
}

struct AIFixRuleTests {
    @Test func suspiciousWhenMarginIsSmall() {
        let result = scored("機関", margins: [0.4, 3])
        #expect(AIFixRule.isSuspicious(result, shown: "機関", reading: "きかん", threshold: 1.0))
    }

    @Test func notSuspiciousWhenMarginIsLarge() {
        let result = scored("機関", margins: [2, 3])
        #expect(!AIFixRule.isSuspicious(result, shown: "機関", reading: "きかん", threshold: 1.0))
    }

    @Test func noCompetitorIsNotSuspicious() {
        let result = scored("あ", margins: [.infinity])
        #expect(!AIFixRule.isSuspicious(result, shown: "あ", reading: "あ", threshold: 1.0))
    }

    /// 学習・ユーザ辞書などで表示が差し替わっていれば、モデルのマージンは表示に当てはまらない
    @Test func ignoresWhenShownDiffersFromModel() {
        let result = scored("機関", margins: [0.1, 0.1])
        #expect(!AIFixRule.isSuspicious(result, shown: "期間", reading: "きかん", threshold: 1.0))
    }

    @Test func ignoresLongReading() {
        let reading = String(repeating: "あ", count: AIFixRule.maximumReadingLength + 1)
        let result = scored(reading, margins: Array(repeating: 0.1, count: reading.count))
        #expect(!AIFixRule.isSuspicious(result, shown: reading, reading: reading, threshold: 1.0))
    }

    @Test func messageUsesKatakanaReading() {
        #expect(AIFixRule.message(context: "", reading: "きかん", conversion: "機関")
            == "前の文脈: （なし）\n読み: キカン\n変換結果: 機関")
    }

    @Test func answerTakesLastLine() {
        #expect(AIFixRule.answer(from: "「機関」は文脈に合いません。\n期間", current: "機関") == "期間")
        #expect(AIFixRule.answer(from: "  期間  \n\n", current: "機関") == "期間")
    }

    @Test func feedbackMarksChangedPart() {
        let feedback = AIFixRule.feedback(original: "研究機関を延長する", corrected: "研究期間を延長する")
        #expect(feedback == TypoCorrectionFeedback(
            prefix: "研究", originalChanged: "機関", correctedChanged: "期間", suffix: "を延長する"))
        #expect(AIFixRule.feedback(original: "同じ", corrected: "同じ") == nil)
    }

    @Test func feedbackHandlesInsertion() {
        let feedback = AIFixRule.feedback(original: "ああ", corrected: "あいあ")
        #expect(feedback?.original == "ああ")
        #expect(feedback?.corrected == "あいあ")
    }

    @Test func answerRejectsEmptyAndTooLong() {
        #expect(AIFixRule.answer(from: " \n ", current: "機関") == nil)
        #expect(AIFixRule.answer(from: "前の文脈をすべて書き写した長い答え", current: "機関") == nil)
    }
}
