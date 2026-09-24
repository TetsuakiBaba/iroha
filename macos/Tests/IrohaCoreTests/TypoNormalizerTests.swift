import XCTest
@testable import IrohaCore

/// 打ち間違い訂正モデル（`training/typo-normalizer`）の Swift 移植が
/// 学習側（PyTorch）と同じ数を出すかを、書き出しに付いてくる `parity.json` で確かめる。
///
/// SWIFT-PORT.md §7 の順序（ロジット → greedy → logP / margin）に沿って見る。
/// **生成が一致してもロジットがずれていれば実装は間違っている**ので、ロジットから確認する。
///
/// ここでは時間の都合で先頭 40 件だけを見る。モデルを差し替えたときは 200 件全部を
/// `iroha-cli typo parity` で回すこと（`typo eval` で訂正率・過剰訂正率の表も出る）
final class TypoNormalizerTests: XCTestCase {

    private struct ParityCase: Decodable {
        let noisy: String
        let clean: String
        let greedy: String
        let logprobGreedy: Double
        let logprobNoisy: Double
        let margin: Double
        let firstLogits: [[Float]]

        enum CodingKeys: String, CodingKey {
            case noisy, clean, greedy, margin
            case logprobGreedy = "logprob_greedy"
            case logprobNoisy = "logprob_noisy"
            case firstLogits = "first_logits"
        }
    }

    private struct ParityFile: Decodable {
        let cases: [ParityCase]
    }

    private static let caseLimit = 40

    /// 許容幅は重みの精度で変える（SWIFT-PORT.md §7）。parity.json は float32 で作った期待値なので、
    /// 同梱するモデルを float16 に落とすと誤差が一段大きくなる（§7-5 の「greedy が数件ずれるのは許容、
    /// margin が 0.1 以上ずれるなら float32 に戻す」がそのときの基準）
    private struct Tolerance {
        let logit: Float
        let logProbability: Double
        let allowedGreedyMismatches: Int
    }

    private func loadParity() throws -> (TypoNormalizer, [ParityCase], Tolerance) {
        guard let directory = TypoNormalizer.defaultDirectoryURL() else {
            throw XCTSkip("Typo Normalizer のモデルが無いためスキップ（IROHA_TYPO_MODEL で指定）")
        }
        let parityURL = directory.appendingPathComponent("parity.json")
        guard let data = try? Data(contentsOf: parityURL) else {
            throw XCTSkip("parity.json が無いためスキップ: \(parityURL.path)")
        }
        let parity = try JSONDecoder().decode(ParityFile.self, from: data)
        let manifestData = try Data(contentsOf: directory.appendingPathComponent("manifest.json"))
        let dtype = (try JSONSerialization.jsonObject(with: manifestData) as? [String: Any])?["dtype"]
            as? String
        let tolerance = dtype == "float16-le"
            ? Tolerance(logit: 0.05, logProbability: 0.1, allowedGreedyMismatches: Self.caseLimit / 50 + 1)
            : Tolerance(logit: 1e-3, logProbability: 0.01, allowedGreedyMismatches: 0)
        return (TypoNormalizer(directory: directory), Array(parity.cases.prefix(Self.caseLimit)), tolerance)
    }

    /// ① teacher forcing のロジット（先頭3ステップ × 120次元）が float32 で 1e-3 以内
    func testLogitsMatchReference() async throws {
        let (normalizer, cases, tolerance) = try loadParity()
        var worst: Float = 0
        for item in cases {
            let logits = try await normalizer.logits(
                source: item.noisy, target: item.clean, steps: item.firstLogits.count)
            XCTAssertEqual(logits.count, item.firstLogits.count, "ステップ数が違う: \(item.noisy)")
            for (step, expected) in item.firstLogits.enumerated() {
                XCTAssertEqual(logits[step].count, expected.count)
                for (index, value) in expected.enumerated() {
                    worst = max(worst, abs(value - logits[step][index]))
                }
            }
        }
        XCTAssertLessThanOrEqual(worst, tolerance.logit, "ロジットの最大誤差 \(worst)")
    }

    /// ② greedy の生成結果が一致する（float32 なら 1 件残らず）
    func testGreedyMatchesReference() async throws {
        let (normalizer, cases, tolerance) = try loadParity()
        var mismatches: [String] = []
        for item in cases {
            let generated = try await normalizer.generate(for: item.noisy)
            if generated != item.greedy {
                mismatches.append("\(item.noisy) → \(generated ?? "nil")（期待 \(item.greedy)）")
            }
        }
        XCTAssertLessThanOrEqual(
            mismatches.count, tolerance.allowedGreedyMismatches,
            "greedy が一致しない: \(mismatches.joined(separator: " / "))")
    }

    /// ③ logP と margin が 0.01 以内（しきい値の判断に直接効くので厳しく見る）
    func testLogProbabilityAndMarginMatchReference() async throws {
        let (normalizer, cases, tolerance) = try loadParity()
        for item in cases {
            let greedy = try await normalizer.logProbability(of: item.greedy, given: item.noisy)
            let noisy = try await normalizer.logProbability(of: item.noisy, given: item.noisy)
            let accuracy = tolerance.logProbability
            XCTAssertEqual(greedy, item.logprobGreedy, accuracy: accuracy, "入力: \(item.noisy)")
            XCTAssertEqual(noisy, item.logprobNoisy, accuracy: accuracy, "入力: \(item.noisy)")
            XCTAssertEqual(greedy - noisy, item.margin, accuracy: accuracy, "入力: \(item.noisy)")
        }
    }

    /// しきい値を超えた訂正だけが返る。θ を上げれば同じ入力でも返らなくなる
    func testCorrectionRespectsThreshold() async throws {
        let (normalizer, cases, _) = try loadParity()
        guard let item = cases.first(where: { $0.greedy != $0.noisy && $0.margin > 2.0 }) else {
            throw XCTSkip("しきい値を超える訂正の例が先頭 \(Self.caseLimit) 件に無い")
        }
        let accepted = try await normalizer.correction(for: item.noisy, threshold: 2.0)
        XCTAssertEqual(accepted?.corrected, item.greedy)
        XCTAssertEqual(accepted?.reading, item.noisy)
        let rejected = try await normalizer.correction(for: item.noisy, threshold: item.margin + 1)
        XCTAssertNil(rejected, "θ を margin より上げたら訂正は返らないはず")
    }

    /// 訂正が出ないケース（モデルが入力をそのまま返す）は nil
    func testNoCorrectionForCleanInput() async throws {
        let (normalizer, cases, _) = try loadParity()
        guard let item = cases.first(where: { $0.greedy == $0.noisy }) else {
            throw XCTSkip("訂正なしの例が先頭 \(Self.caseLimit) 件に無い")
        }
        let correction = try await normalizer.correction(for: item.noisy, threshold: 2.0)
        XCTAssertNil(correction)
    }

    /// 対象外の読み（長すぎる・語彙に無い文字）はモデルを呼ばずに見送る
    func testUnsupportedReadings() async throws {
        guard let directory = TypoNormalizer.defaultDirectoryURL() else {
            throw XCTSkip("Typo Normalizer のモデルが無いためスキップ")
        }
        let normalizer = TypoNormalizer(directory: directory)
        // XCTAssert のオートクロージャは await を取れないので、いったん受けてから確かめる
        let tooLong = String(repeating: "あ", count: TypoNormalizer.maxReadingLength + 1)
        let supportsTooLong = try await normalizer.supports(reading: tooLong)
        XCTAssertFalse(supportsTooLong)
        let correctionForTooLong = try await normalizer.correction(for: tooLong)
        XCTAssertNil(correctionForTooLong)
        // 漢字・カタカナ・ゐ ゑ は語彙に無い（かな漢字変換の前段なので読みだけを見る）
        for reading in ["漢字", "カタカナ", "ゐゑ", ""] {
            let supported = try await normalizer.supports(reading: reading)
            XCTAssertFalse(supported, "対象外のはず: \(reading)")
        }
        let supportsHiragana = try await normalizer.supports(reading: "こんにちは")
        XCTAssertTrue(supportsHiragana)
    }

    /// float16 の展開が 65,536 通り全部で `Float16` と一致する（`iroha-cli typo shrink` で配るので、
    /// 非正規化数（|x| < 2^-14）の指数を 1 つ間違えるといった取りこぼしが実際の重みに乗る）
    func testHalfToFloatMatchesFloat16() throws {
        #if arch(arm64)
        for bits in 0...UInt16.max {
            let expected = Float(Float16(bitPattern: bits))
            let actual = TypoWeightConversion.halfToFloat(bits)
            if expected.isNaN {
                XCTAssertTrue(actual.isNaN, "bits=\(bits)")
            } else {
                XCTAssertEqual(actual.bitPattern, expected.bitPattern, "bits=\(bits) → \(actual) / \(expected)")
            }
        }
        #else
        throw XCTSkip("Float16 と比べられないアーキテクチャ")
        #endif
    }

    /// float32 → float16 → float32 の往復が `Float16` の丸めと一致する
    func testFloatToHalfMatchesFloat16() throws {
        #if arch(arm64)
        let values: [Float] = [0, -0, 1, -1, 0.5, 3.14159, -3.75, 65504, 65520, 1e-4, 6.0e-5,
                               3.05e-5, 1.5e-5, 5.96e-8, 1e-8, -1e-8, 1e10, -1e10, 0.1, -0.2]
        for value in values {
            let expected = Float16(value).bitPattern
            XCTAssertEqual(TypoWeightConversion.floatToHalf(value), expected, "value=\(value)")
        }
        #else
        throw XCTSkip("Float16 と比べられないアーキテクチャ")
        #endif
    }
}
