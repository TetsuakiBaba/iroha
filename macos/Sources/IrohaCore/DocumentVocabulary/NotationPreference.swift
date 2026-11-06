import Foundation

/// 文書から数えた「表記の好み」: 読みごとに、著者がどの表記を何回使ったか。
///
/// 論文の文を変換すると、間違いの多くは語の誤りではなく、かなと漢字の書き分け
/// （及び／および、主に／おもに、見られた／みられた）だった（2026-11-07、論文 12 本・400 文で
/// 間違えた 164 文のうち 103 文）。こうした書き分けはどちらも正しい日本語で、モデルが一般の文章から
/// 学んだ傾向と、その人の書き方が違うことで起きる。
///
/// 数える単位は分かち書きの 1 語（`ReadingToken`）。macOS の分かち書きは表記が違っても同じ単位に
/// 分ける（「見/られ/た」と「み/られ/た」、「切り替え」と「切替え」）。
///
/// 同じ読みの語がすべて表記の違いとは限らない（精度／制度、用／よう）。表記の違いとみなす条件は
/// `areVariants` にまとめた（漢字の包含関係と送り仮名の有無で決める）
public struct NotationPreference: Codable, Sendable, Equatable {

    /// 読み → 表記 → 回数
    public private(set) var counts: [String: [String: Int]]

    public static let empty = NotationPreference(counts: [:])

    public init(counts: [String: [String: Int]]) {
        self.counts = counts
    }

    /// 分かち書きした文の並びから数える
    public init(sentences: [[ReadingToken]]) {
        var counts: [String: [String: Int]] = [:]
        for tokens in sentences {
            for token in tokens {
                guard let reading = token.reading, Self.isCountable(token.surface) else { continue }
                counts[reading, default: [:]][token.surface, default: 0] += 1
            }
        }
        self.counts = counts
    }

    /// 足し合わせる（文書ごとに数えたものをまとめる）
    public func adding(_ other: NotationPreference) -> NotationPreference {
        var merged = counts
        for (reading, surfaces) in other.counts {
            for (surface, count) in surfaces { merged[reading, default: [:]][surface, default: 0] += count }
        }
        return NotationPreference(counts: merged)
    }

    /// 差し引く（評価で、試す文の文書で数えた分を除く）
    public func subtracting(_ other: NotationPreference) -> NotationPreference {
        var result = counts
        for (reading, surfaces) in other.counts {
            for (surface, count) in surfaces {
                let remaining = (result[reading]?[surface] ?? 0) - count
                result[reading]?[surface] = remaining > 0 ? remaining : nil
            }
            if result[reading]?.isEmpty == true { result[reading] = nil }
        }
        return NotationPreference(counts: result)
    }

    /// 表記が 2 つ以上ある読み（表記の違いがあるかは問わない）の数
    public var readingsWithSeveralSurfaces: Int {
        counts.values.filter { $0.count >= 2 }.count
    }

    /// 表記の違いがある読みと、その表記・回数（調査用。回数の多い読みの順）
    public func variantGroups(minimumTotal: Int = 2) -> [(reading: String, surfaces: [(surface: String, count: Int)])] {
        var groups: [(String, [(String, Int)], Int)] = []
        for (reading, surfaces) in counts where surfaces.count >= 2 {
            let sorted = surfaces.sorted { $0.value != $1.value ? $0.value > $1.value : $0.key < $1.key }
            let hasVariant = sorted.indices.contains { i in
                sorted.indices.contains { j in i < j && Self.areVariants(sorted[i].key, sorted[j].key) }
            }
            let total = sorted.reduce(0) { $0 + $1.value }
            guard hasVariant, total >= minimumTotal else { continue }
            groups.append((reading, sorted.map { ($0.key, $0.value) }, total))
        }
        return groups.sorted { $0.2 != $1.2 ? $0.2 > $1.2 : $0.0 < $1.0 }.map { (reading: $0.0, surfaces: $0.1) }
    }

    /// 表記 `surface`（読み `reading`）を、その人がより多く使う表記に替えるならその表記。
    ///
    /// `surface` と表記の違いにあたる表記（`surface` 自身を含む）の回数の合計のうち、
    /// 最も多い表記が `minimumShare` 以上を占め、かつ `minimumCount` 回以上使われていて、
    /// それが `surface` と違うときに返す。替えないなら nil
    public func preferredSurface(
        for surface: String, reading: String, minimumCount: Int = 2, minimumShare: Double = 0.5
    ) -> String? {
        // 読みが 1 文字の語（み／見、た／他）は、別の語とまぎれやすいので替えない
        guard reading.count >= 2, let surfaces = counts[reading] else { return nil }
        let variants = surfaces.filter { $0.key == surface || Self.areVariants($0.key, surface) }
        let total = variants.values.reduce(0, +)
        guard total > 0,
              let best = variants.max(by: { $0.value != $1.value ? $0.value < $1.value : $0.key > $1.key }),
              best.key != surface, best.value >= minimumCount,
              Double(best.value) / Double(total) >= minimumShare
        else { return nil }
        return best.key
    }

    /// 分かち書きした文を、その人の表記の好みで書き換える（変換結果に当てる）
    public func rewrite(
        _ tokens: [ReadingToken], minimumCount: Int = 2, minimumShare: Double = 0.5
    ) -> String {
        tokens.map { token in
            guard let reading = token.reading, Self.isCountable(token.surface) else { return token.surface }
            return preferredSurface(
                for: token.surface, reading: reading, minimumCount: minimumCount, minimumShare: minimumShare)
                ?? token.surface
        }.joined()
    }

    /// 2 つの表記が「同じ語の表記の違い」か。
    ///
    /// - 両方に漢字があるとき: 片方の漢字がもう片方の漢字にすべて含まれる（切替え／切り替え）
    /// - 漢字の表記とかなの表記: 漢字の表記に送り仮名があり、かなの表記がひらがなのとき
    ///   （及び／および、行う／おこなう、挙げ／あげ）。送り仮名のない漢字 1 字の語とかなの組
    ///   （用／よう、内／ない、図／ず、中／なか）は、機能語と名詞のように別の語であることが多いので含めない
    /// - かなどうし: ひらがなとカタカナの書き分けで 3 文字以上のとき（きらきら／キラキラ）
    ///
    /// 品詞が分からないので、これでも別の語を含む（鳴る／なる、上げ／あげ と 挙げ／あげ）。
    /// 2026-11-07 の計測で、書き換えて壊れる文の数を見て決めた
    public static func areVariants(_ a: String, _ b: String) -> Bool {
        guard a != b else { return false }
        let kanjiA = Set(a.unicodeScalars.filter(DocumentText.isKanji))
        let kanjiB = Set(b.unicodeScalars.filter(DocumentText.isKanji))
        guard kanjiA.isSubset(of: kanjiB) || kanjiB.isSubset(of: kanjiA) else { return false }
        func isHiraganaOnly(_ text: String) -> Bool { text.unicodeScalars.allSatisfy(DocumentText.isHiragana) }
        func isKatakanaOnly(_ text: String) -> Bool { text.unicodeScalars.allSatisfy(DocumentText.isKatakana) }
        switch (kanjiA.isEmpty, kanjiB.isEmpty) {
        case (false, false):
            return true
        case (true, true):
            guard a.count >= 3 else { return false }
            return (isHiraganaOnly(a) && isKatakanaOnly(b)) || (isKatakanaOnly(a) && isHiraganaOnly(b))
        default:
            let (kana, kanji) = kanjiA.isEmpty ? (a, b) : (b, a)
            return isHiraganaOnly(kana) && kanji.unicodeScalars.contains(where: DocumentText.isHiragana)
        }
    }

    /// 数える語か（かな・漢字だけでできている語。句読点・英字・数字・記号は数えない）
    static func isCountable(_ surface: String) -> Bool {
        !surface.isEmpty && surface.unicodeScalars.allSatisfy {
            DocumentText.isKanji($0) || DocumentText.isHiragana($0) || DocumentText.isKatakana($0)
        }
    }
}
