import XCTest
@testable import IrohaCore

final class ConfidenceAlignerTests: XCTestCase {
    private func confidence(_ logProb: Float, margin: Float = .infinity, relaxed: Bool = false) -> CharacterConfidence {
        CharacterConfidence(logProb: logProb, margin: margin, relaxed: relaxed)
    }

    func testOneTokenPerCharacter() {
        var aligner = ConfidenceAligner()
        aligner.append(Data("今".utf8), confidence: confidence(-0.1, margin: 3))
        aligner.append(Data("日".utf8), confidence: confidence(-0.2, margin: 1))
        let result = aligner.aligned(untrimmed: "今日", trimmed: "今日")
        XCTAssertEqual(result.map(\.margin), [3, 1])
    }

    func testTokenSpanningSeveralCharacters() {
        var aligner = ConfidenceAligner()
        aligner.append(Data("天気".utf8), confidence: confidence(-0.5, margin: 2))
        let result = aligner.aligned(untrimmed: "天気", trimmed: "天気")
        XCTAssertEqual(result.count, 2)
        XCTAssertEqual(result.map(\.margin), [2, 2])
    }

    func testByteTokensSplittingOneCharacterTakeTheWeakestValue() {
        // 「記」(E8 A8 98) が3つのバイトトークンで出た: 文字の自信度は最小のマージン・最小の対数確率
        var aligner = ConfidenceAligner()
        let bytes = Array("記".utf8)
        aligner.append(Data([bytes[0]]), confidence: confidence(-0.1, margin: 5))
        aligner.append(Data([bytes[1]]), confidence: confidence(-1.5, margin: 0.3, relaxed: true))
        aligner.append(Data([bytes[2]]), confidence: confidence(-0.2, margin: 4))
        aligner.append(Data("者".utf8), confidence: confidence(-0.3, margin: 2))
        let result = aligner.aligned(untrimmed: "記者", trimmed: "記者")
        XCTAssertEqual(result.count, 2)
        XCTAssertEqual(result[0], confidence(-1.5, margin: 0.3, relaxed: true))
        XCTAssertEqual(result[1], confidence(-0.3, margin: 2))
    }

    func testTrimmingDropsLeadingWhitespaceAndTrailingFragment() {
        var aligner = ConfidenceAligner()
        aligner.append(Data(" ".utf8), confidence: confidence(-2, margin: 0.1))
        aligner.append(Data("私".utf8), confidence: confidence(-0.1, margin: 3))
        aligner.append(Data([Array("は".utf8)[0]]), confidence: confidence(-0.4, margin: 1))  // 途中で終わった断片
        let result = aligner.aligned(untrimmed: " 私", trimmed: "私")
        XCTAssertEqual(result.map(\.margin), [3])
    }
}
