import Foundation
import IrohaCore

/// macOS の `CFStringTokenizer`（日本語）で分かち書きし、読みを付ける。
///
/// 読みは `kCFStringTokenizerAttributeLatinTranscription`（かなに忠実なローマ字。「を」は wo、
/// 「おお」と「おう」も書き分ける）をひらがなに戻して作る。2026-11-07 に確かめた範囲では
/// 一般の語・専門用語はほぼ正しく、人名などの固有名詞は間違えることがある（「立石」→ りっしゃく）。
/// 読みが 1 つに決まらない語は 1 つを選ぶ（「日本」→ にっぽん）。
///
/// - ひらがなだけの語は表記をそのまま読みにする
/// - カタカナだけの語（「ー」を含む）はひらがなに直す（ローマ字を経ると長音が崩れるため）
/// - 漢字を含む語はローマ字からひらがなにする。ひらがなにならない文字が残ったら読みなし
/// - 読点（、，）は「、」、句点（。．）は「。」を読みにする（iroha で打つ記号）
/// - それ以外（英字・数字・記号）は読みなし
public struct SystemReadingTokenizer: ReadingTokenizer {

    public init() {}

    public func tokens(in text: String) -> [ReadingToken] {
        guard !text.isEmpty else { return [] }
        let string = text as NSString
        let cfString = text as CFString
        guard let tokenizer = CFStringTokenizerCreate(
            nil, cfString, CFRangeMake(0, string.length), kCFStringTokenizerUnitWordBoundary,
            Locale(identifier: "ja") as CFLocale)
        else { return [ReadingToken(surface: text, reading: nil)] }

        var tokens: [ReadingToken] = []
        var position = 0
        while CFStringTokenizerAdvanceToNextToken(tokenizer) != [] {
            let range = CFStringTokenizerGetCurrentTokenRange(tokenizer)
            guard range.location != kCFNotFound, range.length > 0 else { continue }
            // 分かち書きが飛ばした部分（空白など）も表記として残し、つなげると元の文字列に戻るようにする
            if range.location > position {
                let gap = string.substring(with: NSRange(location: position, length: range.location - position))
                tokens.append(ReadingToken(surface: gap, reading: Self.reading(ofPlain: gap)))
            }
            let surface = string.substring(with: NSRange(location: range.location, length: range.length))
            let latin = CFStringTokenizerCopyCurrentTokenAttribute(
                tokenizer, kCFStringTokenizerAttributeLatinTranscription) as? String
            tokens.append(ReadingToken(surface: surface, reading: Self.reading(surface: surface, latin: latin)))
            position = range.location + range.length
        }
        if position < string.length {
            let rest = string.substring(from: position)
            tokens.append(ReadingToken(surface: rest, reading: Self.reading(ofPlain: rest)))
        }
        return tokens
    }

    static func reading(surface: String, latin: String?) -> String? {
        if let plain = reading(ofPlain: surface) { return plain }
        let scalars = surface.unicodeScalars
        if scalars.allSatisfy({ DocumentText.isKatakana($0) }) {
            return katakanaToHiragana(surface)
        }
        guard scalars.contains(where: { DocumentText.isKanji($0) }), let latin, !latin.isEmpty else { return nil }
        let converted = NSMutableString(string: latin.lowercased())
        CFStringTransform(converted, nil, kCFStringTransformLatinHiragana, false)
        let reading = converted as String
        guard reading.unicodeScalars.allSatisfy({ DocumentText.isHiragana($0) }) else { return nil }
        return reading
    }

    /// ひらがな・句読点だけの表記の読み（それ以外は nil）
    static func reading(ofPlain surface: String) -> String? {
        var result = ""
        for character in surface {
            switch character {
            case "、", "，", "､": result.append("、")
            case "。", "．", "｡": result.append("。")
            default:
                guard character.unicodeScalars.allSatisfy({ DocumentText.isHiragana($0) }) else { return nil }
                result.append(character)
            }
        }
        return result.isEmpty ? nil : result
    }

    static func katakanaToHiragana(_ text: String) -> String {
        String(String.UnicodeScalarView(text.unicodeScalars.map { scalar in
            if (0x30A1...0x30F6).contains(scalar.value), let converted = Unicode.Scalar(scalar.value - 0x60) {
                return converted
            }
            return scalar
        }))
    }
}
