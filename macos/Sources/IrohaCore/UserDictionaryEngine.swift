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

    /// ライブ変換・文節分割用（1候補）: 辞書を当てた結果を自信度つきで返す。辞書で埋めた部分は信頼済み。
    /// 出力が読みより大幅に長いエントリ（ハッシュタグ・定型文など）は文中に埋め込まれると
    /// 邪魔なので使わない（候補ウィンドウにだけ出す）
    public func convertScored(reading: String, context: String) async throws -> ScoredConversion {
        let dictionary = dictionaryProvider()
        guard !dictionary.isEmpty, !dictionary.isEmptyForLiveConversion else {
            return try await base.convertScored(reading: reading, context: context)
        }
        if let word = dictionary.liveWords(forReading: reading).first { return .trusted(word) }
        let chunks = dictionary.split(reading, forLiveConversion: true)
        guard Self.hasWordChunk(chunks) else {
            return try await base.convertScored(reading: reading, context: context)
        }
        return try await compose(chunks, context: context)
    }

    public func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        let dictionary = dictionaryProvider()
        guard !dictionary.isEmpty else {
            return try await base.convert(
                reading: reading, context: context, candidateCount: candidateCount)
        }

        if candidateCount <= 1 {
            return [try await convertScored(reading: reading, context: context).text]
        }

        // 候補ウィンドウ用: ユーザ辞書の単語を先頭に、続けてエンジンの候補を並べる
        let exactWords = dictionary.words(forReading: reading)
        let chunks = dictionary.split(reading)
        var results = exactWords
        if Self.hasWordChunk(chunks), exactWords.isEmpty {
            results.append(try await compose(chunks, context: context).text)
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

    private static func hasWordChunk(_ chunks: [UserDictionary.Chunk]) -> Bool {
        chunks.contains { if case .word = $0 { return true } else { return false } }
    }

    /// 辞書一致部分はそのまま、それ以外はエンジンに変換させて連結する。
    /// 直前までの変換結果を次のチャンクの文脈として渡す
    private func compose(_ chunks: [UserDictionary.Chunk], context: String) async throws -> ScoredConversion {
        var result = ScoredConversion.trusted("")
        var context = context
        for chunk in chunks {
            switch chunk {
            case .word(let word):
                result = result.appending(.trusted(word))
                context += word
            case .reading(let reading):
                let converted = try await base.convertScored(reading: reading, context: context)
                result = result.appending(converted)
                context += converted.text
            }
        }
        return result
    }
}
