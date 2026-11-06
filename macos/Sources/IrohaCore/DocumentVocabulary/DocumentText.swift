import Foundation

/// 文書（PDF・Word など）から取り出した文字列を、語を集めたり文に分けたりできる形に整える。
///
/// PDF から取り出した文字列には組版の都合の改行・空白が残る:
/// - 行末の改行が語の途中に入る（「システ⏎ム」「対⏎面授業」）
/// - 和文と英数字の間の空白（「2020 年度」「SSD および」）
/// - 字取りの空白（「馬 場 哲 晃」）
/// 日本語の文章では文字の間の空白・改行は意味を持たないので、和文の文字に接する改行・空白は除く。
/// それ以外（英文の単語の間など）の改行は空白 1 つにする。
public enum DocumentText {

    /// 和文の文字か（ひらがな・カタカナ・漢字・和文の記号・全角の英数字と記号）
    public static func isJapanese(_ scalar: Unicode.Scalar) -> Bool {
        switch scalar.value {
        case 0x3000...0x303F,   // 和文の記号（、。「」々〆 など）
             0x3040...0x309F,   // ひらがな
             0x30A0...0x30FF,   // カタカナ
             0x3400...0x4DBF,   // 漢字（拡張 A）
             0x4E00...0x9FFF,   // 漢字
             0xF900...0xFAFF,   // 互換漢字
             0xFF00...0xFFEF:   // 全角の英数字と記号・半角カタカナ
            return true
        default:
            return false
        }
    }

    public static func isKanji(_ scalar: Unicode.Scalar) -> Bool {
        switch scalar.value {
        case 0x3400...0x4DBF, 0x4E00...0x9FFF, 0xF900...0xFAFF, 0x3005:  // 0x3005 は「々」
            return true
        default:
            return false
        }
    }

    public static func isHiragana(_ scalar: Unicode.Scalar) -> Bool {
        (0x3041...0x3096).contains(scalar.value) || scalar.value == 0x30FC  // 「ー」も読みに含めてよい
    }

    public static func isKatakana(_ scalar: Unicode.Scalar) -> Bool {
        (0x30A1...0x30FA).contains(scalar.value) || scalar.value == 0x30FC
    }

    /// 改行・空白を整える（上の説明のとおり）
    public static func normalize(_ raw: String) -> String {
        let scalars = Array(raw.replacingOccurrences(of: "\r\n", with: "\n").unicodeScalars)
        var result = String.UnicodeScalarView()
        var index = 0
        while index < scalars.count {
            let scalar = scalars[index]
            guard isBreak(scalar) else {
                result.append(scalar)
                index += 1
                continue
            }
            // 改行・空白の並びをまとめて扱う
            var end = index
            while end < scalars.count, isBreak(scalars[end]) { end += 1 }
            let before = result.last
            let after = end < scalars.count ? scalars[end] : nil
            let touchesJapanese = (before.map(isJapanese) ?? false) || (after.map(isJapanese) ?? false)
            if !touchesJapanese, before != nil, after != nil {
                result.append(" ")
            }
            index = end
        }
        return String(result)
    }

    /// 文に分ける。文の終わりの記号（。．！？）は文に含める。
    /// 文の前後の空白は除き、空の文は返さない
    public static func sentences(in text: String) -> [String] {
        var sentences: [String] = []
        var current = ""
        for character in text {
            current.append(character)
            if sentenceTerminators.contains(character) {
                appendTrimmed(current, to: &sentences)
                current = ""
            }
        }
        appendTrimmed(current, to: &sentences)
        return sentences
    }

    /// 論文の参考文献の節（最後の「参考文献」から後ろ）を除く。
    /// 文献の一覧は文として打つものではなく、書名・人名の羅列が語の数え方をゆがめる
    public static func removingReferences(_ text: String) -> String {
        guard let range = text.range(of: "参考文献", options: .backwards) else { return text }
        // 文書の前半にしか無い「参考文献」（本文中で触れただけ）は節の見出しとみなさない
        let offset = text.distance(from: text.startIndex, to: range.lowerBound)
        guard offset * 2 >= text.count else { return text }
        return String(text[..<range.lowerBound])
    }

    private static let sentenceTerminators: Set<Character> = ["。", "．", "！", "？"]

    private static func isBreak(_ scalar: Unicode.Scalar) -> Bool {
        scalar == " " || scalar == "\n" || scalar == "\t" || scalar == "\u{3000}" || scalar == "\r"
            || scalar == "\u{00A0}"
    }

    private static func appendTrimmed(_ sentence: String, to sentences: inout [String]) {
        let trimmed = sentence.trimmingCharacters(in: .whitespacesAndNewlines)
        if !trimmed.isEmpty { sentences.append(trimmed) }
    }
}
