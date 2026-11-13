import Foundation
import KanaKanjiConverterModule

/// 和暦と西暦の年の候補（「2026ねん」→ 令和8年、「しょうわ64ねん」→ 1989年）。
///
/// azooKey の `CalendarSpecialCandidateProvider`（AzooKeyKanaKanjiConverter v0.11.2、MIT）と同じ候補・
/// 同じ評価値を作る。違いは明治の年だけ: 元の実装は 1869〜1911 年で 1967 を引いていて、
/// 「1900ねん」が「明治-67年」になる（1867 の書き間違いとみられる。2026-11-13）。
/// 元の実装の関数は公開されていないので、ここに書き写して直した。
/// Windows 版（`windows/iroha-core/src/lattice/special_candidates.cpp`）も同じ規則にしてある。
/// 変えるときは両方を合わせる
public enum EraYearCandidates {

    public struct Entry: Equatable, Sendable {
        public let text: String
        /// 候補の評価値（辞書ラティスの候補と同じスケール）
        public let value: Float
        /// 真なら和暦（西暦の年から作った候補）、偽なら西暦（和暦の年から作った候補）
        public let isWareki: Bool
    }

    /// `katakana` は読み全体をカタカナにしたもの。和暦の候補、西暦の候補の順に返す
    public static func entries(katakana: String) -> [Entry] {
        wareki(katakana) + seireki(katakana)
    }

    /// 西暦の年（「2026ネン」）から和暦。改元の年は新旧の元号の 2 つを返す
    private static func wareki(_ string: String) -> [Entry] {
        guard let year = Int(string.prefix(4)), string.hasSuffix("ネン") else { return [] }
        func first(_ text: String) -> Entry { Entry(text: text, value: -18, isWareki: true) }
        func second(_ text: String) -> Entry { Entry(text: text, value: -19, isWareki: true) }
        switch year {
        case 1989: return [first("平成元年"), second("昭和64年")]
        case 2019: return [first("令和元年"), second("平成31年")]
        case 1926: return [first("昭和元年"), second("大正15年")]
        case 1912: return [first("大正元年"), second("明治45年")]
        case 1868: return [first("明治元年"), second("慶應4年")]
        case 1990...2018: return [first("平成\(year - 1988)年")]
        case 1927...1988: return [first("昭和\(year - 1925)年")]
        case 1869...1911: return [first("明治\(year - 1867)年")]
        case 1913...1925: return [first("大正\(year - 1911)年")]
        case 2020...: return [first("令和\(year - 2018)年")]
        default: return []
        }
    }

    /// 和暦の年（「レイワ8ネン」「ヘイセイガンネン」）から西暦
    private static func seireki(_ string: String) -> [Entry] {
        func entry(_ year: Int) -> [Entry] { [Entry(text: "\(year)年", value: -15, isWareki: false)] }
        let firstYears = [
            "メイジガンネン": 1868, "タイショウガンネン": 1912, "ショウワガンネン": 1926,
            "ヘイセイガンネン": 1989, "レイワガンネン": 2019,
        ]
        if let year = firstYears[string] { return entry(year) }
        guard string.hasSuffix("ネン") else { return [] }
        let eras: [(name: String, offset: Int)] = [
            ("ショウワ", 1925), ("ヘイセイ", 1988), ("レイワ", 2018), ("メイジ", 1867), ("タイショウ", 1911),
        ]
        for era in eras where string.hasPrefix(era.name) {
            // 年の数字は 1 桁か 2 桁（全体の文字数 = 元号 + 数字 + 「ネン」）
            let digitCount = string.count - era.name.count - 2
            guard digitCount == 1 || digitCount == 2,
                  let year = Int(string.dropFirst(era.name.count).dropLast(2)) else { return [] }
            return entry(year + era.offset)
        }
        return []
    }
}

/// `EraYearCandidates` を辞書ラティスの特別な候補として出す（`LatticeConverter.specialCandidateProviders`）
struct EraYearCandidateProvider: SpecialCandidateProvider {
    func provideCandidates(
        converter _: KanaKanjiConverter, inputData: ComposingText, options _: ConvertRequestOptions
    ) -> [KanaKanjiConverterModule.Candidate] {
        let ruby = hiraganaToKatakana(inputData.convertTarget)
        return EraYearCandidates.entries(katakana: ruby).map { entry in
            // 品詞と意味の ID は元の実装と同じ（和暦は一般名詞・年、西暦は固有名詞・一般）
            let cid = entry.isWareki ? CIDData.一般名詞.cid : CIDData.固有名詞.cid
            let mid = entry.isWareki ? MIDData.年.mid : MIDData.一般.mid
            return KanaKanjiConverterModule.Candidate(
                text: entry.text,
                value: entry.value,
                composingCount: .inputCount(inputData.input.count),
                lastMid: mid,
                data: [DicdataElement(word: entry.text, ruby: ruby, cid: cid, mid: mid, value: entry.value)])
        }
    }
}
