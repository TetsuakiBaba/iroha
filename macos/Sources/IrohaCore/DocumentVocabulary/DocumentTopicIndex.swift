import Foundation

/// 入力中の話題が、どの文書にどれだけ近いか。
///
/// 近さは「入力の話題語のうち、その文書に出てくる語の割合」を、語ごとの idf で重み付けしたもの（0〜1）:
///   Σ_{入力の語 w で文書に出るもの} idf(w) ／ Σ_{入力の語 w} idf(w)
/// idf(w) = log((文書数 + 1) / (w が出る文書数 + 1))。すべての文書に出る語（研究・利用・システム）は 0 で
/// 数に入らず、どの文書にも出ない語（掃除機・天気）は最大の log(文書数 + 1) で分母にだけ入る
/// （話題が文書と違う証拠になる）。話題語は漢字かカタカナを含む 2 文字以上の語。
///
/// 最初は文書全体との TF-IDF の余弦類似度にしたが、数語の入力と数千語の文書を比べるので、話題がぴったり
/// 合っても 0.05〜0.2 にしかならず、重みをかけても効かなかった（2026-11-07）。割合なら合っていれば 1 に近い。
///
/// 使い道は、同じ読みの別の語の選び分け（`NotationPreference.wordPenalty`）を、入力の話題が文書に
/// 近いときだけ効かせること（`NotationPreferenceEngine`）
public struct DocumentTopicIndex: Sendable {

    /// 語 → idf（文書に 1 度でも出た語だけ）
    private let idf: [String: Double]
    /// どの文書にも出ない語の idf
    private let unseenIDF: Double
    /// 文書ごとの話題語
    private let words: [Set<String>]

    public var documentCount: Int { words.count }

    /// - Parameter documents: 文書ごとの分かち書きした文
    public init(documents: [[[ReadingToken]]]) {
        let words = documents.map { sentences in
            Set(sentences.flatMap { $0.map(\.surface) }.filter(Self.isTopicWord))
        }
        var documentFrequency: [String: Int] = [:]
        for set in words { for word in set { documentFrequency[word, default: 0] += 1 } }
        let total = Double(documents.count)
        self.idf = documentFrequency.mapValues { log((total + 1) / (Double($0) + 1)) }
        self.unseenIDF = log(total + 1)
        self.words = words
    }

    /// 入力の語に最も近い文書の番号と近さ。話題語が無い・どの文書とも重ならなければ nil
    public func nearest(_ tokens: [ReadingToken]) -> (index: Int, similarity: Double)? {
        let query = tokens.map(\.surface).filter(Self.isTopicWord)
        let total = query.reduce(0) { $0 + (idf[$1] ?? unseenIDF) }
        guard total > 0 else { return nil }
        var best: (index: Int, similarity: Double)?
        for (index, set) in words.enumerated() {
            let shared = query.reduce(0) { $0 + (set.contains($1) ? (idf[$1] ?? 0) : 0) }
            let similarity = shared / total
            if similarity > (best?.similarity ?? 0) { best = (index, similarity) }
        }
        return best
    }

    /// 話題を表す語か（漢字かカタカナを含み、かな・漢字だけでできた 2 文字以上の語）
    static func isTopicWord(_ surface: String) -> Bool {
        surface.count >= 2 && NotationPreference.isCountable(surface)
            && surface.unicodeScalars.contains { DocumentText.isKanji($0) || (DocumentText.isKatakana($0) && $0.value != 0x30FC) }
    }
}
