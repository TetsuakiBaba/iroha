import Foundation

/// 学習結果（ユーザが修正した変換）を反映させる変換エンジンのデコレータ。
///
/// - 入力全体の読みが過去の修正と一致 → エンジンを呼ばずにその確定文字列を返す
/// - 候補ウィンドウでは学習結果を先頭に並べる
///
/// 一致しなければ何もせず素通しする（文中の一部に当てはめることはしない）。
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
        guard let learned = dictionaryProvider().result(forReading: reading) else {
            return try await base.convert(
                reading: reading, context: context, candidateCount: candidateCount)
        }

        // ライブ変換・第一候補は学習結果をそのまま返す（エンジンを呼ばない）
        if candidateCount <= 1 { return [learned] }

        // 候補ウィンドウ: 学習結果を先頭に、続けてエンジンの候補
        var results = [learned]
        // 学習結果だけでも返せるので、エンジンが失敗しても候補ウィンドウは開く
        if let baseCandidates = try? await base.convert(
            reading: reading, context: context, candidateCount: candidateCount) {
            for candidate in baseCandidates where !results.contains(candidate) {
                results.append(candidate)
            }
        }
        return results
    }
}
