import XCTest
import MLX
@testable import IrohaTrain
@testable import IrohaCore
@testable import IrohaMLX

/// MLX 版のかな漢字変換エンジンが `ZenzEngine`（llama.cpp）と同じ結果を返すか。
/// 計算の順序が違うので f32 で突き合わせる（IME で使う bf16 の一致は AJIMEE で確かめた。
/// `training/t5/MAC-AJIMEE-MLX-2026-10-01.md`）
final class MLXConversionEngineTests: XCTestCase {

    override func setUpWithError() throws {
        try TestSupport.prepareMLX()
    }

    private let inputs: [(reading: String, context: String)] = [
        ("きしゃがきしゃできしゃした", ""),
        ("かんじへんかん", ""),
        ("あしたはあめがふるでしょう", "天気予報によると、"),
        ("ほうこうをかえる", "車の"),
        ("ごおんとほうこう", "鎌倉幕府の"),
        ("わたしはがくせいです", ""),
    ]

    /// 貪欲生成（ライブ変換の第一候補）が ZenzEngine と同じ
    func testGreedyMatchesZenzEngine() async throws {
        let path = try TestSupport.requireT5Model()
        let llama = ZenzEngine(modelPath: path)
        let mlx = MLXConversionEngine(modelPath: path, usesFloat32: true)
        for input in inputs {
            let expected = try await llama.convert(reading: input.reading, context: input.context, candidateCount: 1)
            let actual = try await mlx.convert(reading: input.reading, context: input.context, candidateCount: 1)
            XCTAssertEqual(actual, expected, input.reading)
        }
    }

    /// n-best（辞書ラティスが使えないときの候補ウィンドウ）が ZenzEngine と同じ
    func testNBestMatchesZenzEngine() async throws {
        let path = try TestSupport.requireT5Model()
        let llama = ZenzEngine(modelPath: path)
        let mlx = MLXConversionEngine(modelPath: path, usesFloat32: true)
        for input in inputs.prefix(3) {
            let expected = try await llama.convert(reading: input.reading, context: input.context, candidateCount: 5)
            let actual = try await mlx.convert(reading: input.reading, context: input.context, candidateCount: 5)
            XCTAssertEqual(actual, expected, input.reading)
        }
    }

    /// 候補の一括採点（辞書ラティスの並べ替え）が ZenzEngine と同じ値。空文字列は -inf
    func testScoresMatchZenzEngine() async throws {
        let path = try TestSupport.requireT5Model()
        let llama = ZenzEngine(modelPath: path)
        let mlx = MLXConversionEngine(modelPath: path, usesFloat32: true)
        // 空文字列は先頭に置く（ZenzEngine は先頭以外の空文字列を扱えない。辞書ラティスは空の候補を渡さない）
        let candidates = ["", "記者が汽車で帰社した", "貴社が記者で帰社した", "汽車", "きしゃがきしゃできしゃした"]
        let expected = try await llama.score(candidates: candidates, reading: "きしゃがきしゃできしゃした", context: "本日は")
        let actual = try await mlx.score(candidates: candidates, reading: "きしゃがきしゃできしゃした", context: "本日は")
        XCTAssertEqual(actual.count, expected.count)
        for (index, (a, e)) in zip(actual, expected).enumerated() {
            if e == -.infinity {
                XCTAssertEqual(a, -.infinity, "候補 \(index)")
            } else {
                XCTAssertEqual(a, e, accuracy: 0.01, "候補 \(index)")
            }
        }
        XCTAssertEqual(expected.indices.sorted { expected[$0] > expected[$1] },
                       actual.indices.sorted { actual[$0] > actual[$1] }, "並び順")
    }

    /// bf16（IME の既定）でも貪欲生成の結果は同じ
    func testBFloat16GreedyMatchesZenzEngine() async throws {
        let path = try TestSupport.requireT5Model()
        let llama = ZenzEngine(modelPath: path)
        let mlx = MLXConversionEngine(modelPath: path)
        for input in inputs {
            let expected = try await llama.convert(reading: input.reading, context: input.context, candidateCount: 1)
            let actual = try await mlx.convert(reading: input.reading, context: input.context, candidateCount: 1)
            XCTAssertEqual(actual, expected, input.reading)
        }
    }

    /// T5 以外（zenz = GPT-2）は読めない。黙って別のエンジンで動かさずに失敗する
    func testRejectsNonT5Model() async throws {
        let path = try TestSupport.requireModel()
        XCTAssertFalse(MLXConversionEngine.supports(modelPath: path))
        let mlx = MLXConversionEngine(modelPath: path)
        do {
            _ = try await mlx.convert(reading: "かんじ", context: "", candidateCount: 1)
            XCTFail("T5 以外のモデルで変換できてしまった")
        } catch ConversionError.modelLoadFailed {
        }
    }

    func testMissingModelThrowsModelNotFound() async throws {
        let mlx = MLXConversionEngine(modelPath: "/nonexistent/model.gguf")
        do {
            _ = try await mlx.convert(reading: "かんじ", context: "", candidateCount: 1)
            XCTFail("無いモデルで変換できてしまった")
        } catch ConversionError.modelNotFound(let path) {
            XCTAssertEqual(path, "/nonexistent/model.gguf")
        }
    }
}
