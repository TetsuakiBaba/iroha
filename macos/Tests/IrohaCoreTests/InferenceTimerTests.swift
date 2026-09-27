import XCTest
@testable import IrohaCore

/// NN の計算の代わりに少し眠り、その前後を `InferenceTimer` で測るダミーエンジン（actor）
private actor SleepingEngine: ConversionEngine {
    private(set) var calls = 0

    func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        calls += 1
        return InferenceTimer.measureNeuralNetwork {
            Thread.sleep(forTimeInterval: 0.002)
            return [hiraganaToKatakana(reading)]
        }
    }
}

final class InferenceTimerTests: XCTestCase {

    func testRecordsNothingWithoutCurrent() async throws {
        XCTAssertNil(InferenceTimer.current)
        let result = try await SleepingEngine().convert(reading: "きしゃ", context: "", candidateCount: 1)
        XCTAssertEqual(result, ["キシャ"])
    }

    /// タスクローカル値はデコレータ（別の actor）をまたいで NN の actor まで届く
    func testMeasuresNeuralNetworkThroughDecorators() async throws {
        let timer = InferenceTimer()
        let engine = ChunkedConversionEngine(base: SleepingEngine(), maxChunkLength: 10)
        _ = try await InferenceTimer.$current.withValue(timer) {
            try await engine.convert(reading: "きしゃ", context: "", candidateCount: 1)
        }
        let snapshot = timer.snapshot
        XCTAssertEqual(snapshot.neuralNetworkCalls, 1)
        XCTAssertGreaterThanOrEqual(snapshot.neuralNetwork, .milliseconds(2))
        XCTAssertEqual(snapshot.modelLoad, .zero)
        XCTAssertTrue(snapshot.shortcuts.isEmpty)
    }

    /// 読み全体が学習と一致したら NN を通さず、その理由が残る
    func testNotesLearningShortcut() async throws {
        let base = SleepingEngine()
        let dictionary = LearningDictionary(entries: [LearningEntry(reading: "きしゃ", result: "貴社")])
        let engine = LearningEngine(base: base, dictionary: { dictionary })
        let timer = InferenceTimer()
        let result = try await InferenceTimer.$current.withValue(timer) {
            try await engine.convert(reading: "きしゃ", context: "", candidateCount: 1)
        }
        XCTAssertEqual(result, ["貴社"])
        XCTAssertEqual(timer.snapshot.neuralNetworkCalls, 0)
        XCTAssertEqual(timer.snapshot.shortcuts, [.learning])
    }

    /// 長い読みの区切りがキャッシュに当たったら NN の回数は増えず、その理由が残る
    func testNotesChunkCache() async throws {
        let engine = ChunkedConversionEngine(base: SleepingEngine(), maxChunkLength: 10)
        let reading = "あいうえお、かきくけこさしすせそ"
        _ = try await engine.convert(reading: reading, context: "", candidateCount: 1)

        let timer = InferenceTimer()
        _ = try await InferenceTimer.$current.withValue(timer) {
            try await engine.convert(reading: reading, context: "", candidateCount: 1)
        }
        XCTAssertEqual(timer.snapshot.neuralNetworkCalls, 0)
        XCTAssertEqual(timer.snapshot.shortcuts, [.cache])
    }

    /// 切り離したタスクにはタスクローカル値が引き継がれない（呼び出し側で置き直す必要がある）
    func testDetachedTaskDoesNotInherit() async throws {
        let timer = InferenceTimer()
        await InferenceTimer.$current.withValue(timer) {
            await Task.detached {
                XCTAssertNil(InferenceTimer.current)
            }.value
        }
    }
}
