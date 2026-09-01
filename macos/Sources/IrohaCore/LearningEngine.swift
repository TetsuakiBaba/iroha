import Foundation

/// 学習結果（ユーザが修正した変換）を反映させる変換エンジンのデコレータ。
///
/// - 入力全体の読みが過去の修正と一致 → エンジンを呼ばずにその確定文字列を返す
/// - 入力の一部が一致 → 左から順に、一致部分は学習結果で埋め、残りをエンジンに変換させる。
///   同じ読みでも位置で正解が違う（「きしゃのきしゃ」→「記者の貴社」）ので、
///   文節の学習は「直前までに確定した文字列」が一致するときだけ適用する
/// - 候補ウィンドウでは学習結果を先頭に並べる
///
/// 学習が空のときは何もせず素通しする。
public final class LearningEngine: ConversionEngine {

    private let base: any ConversionEngine
    private let dictionaryProvider: @Sendable () -> LearningDictionary

    public init(
        base: any ConversionEngine,
        dictionary: @escaping @Sendable () -> LearningDictionary = {
            LearningStore.shared.current
        }
    ) {
        self.base = base
        self.dictionaryProvider = dictionary
    }

    public func prewarm() async throws {
        try await base.prewarm()
    }

    public func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        let dictionary = dictionaryProvider()
        guard !dictionary.isEmpty else {
            return try await base.convert(
                reading: reading, context: context, candidateCount: candidateCount)
        }

        if candidateCount <= 1 {
            // 入力全体を過去に確定していればそれをそのまま返す（エンジンを呼ばない）
            if let sentence = dictionary.sentence(forReading: reading) { return [sentence] }
            return [try await compose(reading: reading, context: context, dictionary: dictionary)]
        }

        // 候補ウィンドウ: この読みで学習済みの変換を先頭に、続けてエンジンの候補
        var results = dictionary.learnedResults(forReading: reading, leftContext: context)
        do {
            let baseCandidates = try await base.convert(
                reading: reading, context: context, candidateCount: candidateCount)
            for candidate in baseCandidates where !results.contains(candidate) {
                results.append(candidate)
            }
        } catch {
            if results.isEmpty { throw error }
        }
        return results
    }

    /// 学習済みの文節で埋めながら左から変換する。
    ///
    /// 文脈の条件を判定するには直前までの変換結果が要るので、学習済みの読みに
    /// ぶつかった時点で、そこまでの未変換部分を先にエンジンへ渡して確定させる
    private func compose(
        reading: String, context: String, dictionary: LearningDictionary
    ) async throws -> String {
        let characters = Array(reading)
        var result = ""       // 変換済みの部分（文脈にもなる）
        var pending = ""      // まだエンジンに渡していない読み
        var index = 0

        while index < characters.count {
            if dictionary.mayMatch(characters, from: index, atStart: index == 0) {
                if !pending.isEmpty {
                    result += try await convertChunk(pending, context: context + result)
                    pending = ""
                }
                if let match = dictionary.bestMatch(
                    characters, from: index, leftContext: result, atStart: index == 0) {
                    result += match.result
                    index += match.reading.count
                    continue
                }
            }
            pending.append(characters[index])
            index += 1
        }
        if !pending.isEmpty {
            result += try await convertChunk(pending, context: context + result)
        }
        return result
    }

    private func convertChunk(_ reading: String, context: String) async throws -> String {
        try await base.convert(reading: reading, context: context, candidateCount: 1).first ?? reading
    }
}
