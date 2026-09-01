import Foundation

/// ユーザ辞書を反映させる変換エンジンのデコレータ。
///
/// LLMベースの変換エンジンには辞書を後から差し込む口がないため、読みの側で処理する:
/// - 読み全体がユーザ辞書に完全一致 → その単語を最優先の候補にする
/// - 読みの一部が一致 → 一致部分を単語で埋め、残りだけをエンジンに変換させて連結する
///   （例:「きららざかにいく」→「雲母坂」+ エンジンによる「にいく」→「に行く」）
///
/// ユーザ辞書が空のときは何もせず素通しする（既定のふるまいと完全に同じ）。
public final class UserDictionaryEngine: ConversionEngine {

    private let base: any ConversionEngine
    private let dictionaryProvider: @Sendable () -> UserDictionary

    public init(
        base: any ConversionEngine,
        dictionary: @escaping @Sendable () -> UserDictionary = {
            UserDictionaryStore.shared.current
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

        let exactWords = dictionary.words(forReading: reading)
        let chunks = dictionary.split(reading)
        let hasWordChunk = chunks.contains { if case .word = $0 { return true } else { return false } }

        // ライブ変換・文節分割用（1候補）: 辞書を当てた結果をそのまま返す
        if candidateCount <= 1 {
            if let word = exactWords.first { return [word] }
            guard hasWordChunk else {
                return try await base.convert(reading: reading, context: context, candidateCount: 1)
            }
            return [try await compose(chunks, context: context)]
        }

        // 候補ウィンドウ用: ユーザ辞書の単語を先頭に、続けてエンジンの候補を並べる
        var results = exactWords
        if hasWordChunk, exactWords.isEmpty {
            results.append(try await compose(chunks, context: context))
        }
        do {
            let baseCandidates = try await base.convert(
                reading: reading, context: context, candidateCount: candidateCount)
            for candidate in baseCandidates where !results.contains(candidate) {
                results.append(candidate)
            }
        } catch {
            // エンジンが失敗してもユーザ辞書の単語だけは出す
            if results.isEmpty { throw error }
        }
        return results
    }

    /// 辞書一致部分はそのまま、それ以外はエンジンに変換させて連結する。
    /// 直前までの変換結果を次のチャンクの文脈として渡す
    private func compose(_ chunks: [UserDictionary.Chunk], context: String) async throws -> String {
        var result = ""
        var context = context
        for chunk in chunks {
            switch chunk {
            case .word(let word):
                result += word
                context += word
            case .reading(let reading):
                let converted = try await base.convert(
                    reading: reading, context: context, candidateCount: 1).first ?? reading
                result += converted
                context += converted
            }
        }
        return result
    }
}
