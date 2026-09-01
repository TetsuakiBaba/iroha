import Foundation

/// カタカナ→ひらがな変換（hiraganaToKatakanaの逆）
public func katakanaToHiragana(_ katakana: String) -> String {
    String(katakana.unicodeScalars.map { scalar -> Character in
        // ァ(U+30A1)〜ヶ(U+30F6) → ぁ(U+3041)〜ゖ(U+3096)
        if (0x30A1...0x30F6).contains(scalar.value),
           let converted = Unicode.Scalar(scalar.value - 0x60) {
            return Character(converted)
        }
        return Character(scalar)
    })
}

/// 読み（ひらがな）と変換結果（かな漢字混じり）を突き合わせて文節に分割する。
///
/// 変換結果のひらがな部分（助詞・送り仮名）は読みの中にそのまま現れることを利用し、
/// それらをアンカーとして漢字部分の読み区間を特定する。
/// 例: 読み「きょうはいいてんきですね」 変換「今日はいい天気ですね」
///   → [(きょうはいい, 今日はいい), (てんきですね, 天気ですね)]
///
/// 文節境界の初期推定用であり、正確な形態素解析ではない。
/// アライメントに失敗した場合は全体を1文節として返す。
public enum ReadingAligner {

    public struct Segment: Equatable, Sendable {
        public var reading: String
        public var conversion: String

        public init(reading: String, conversion: String) {
            self.reading = reading
            self.conversion = conversion
        }
    }

    public static func segmentReading(_ reading: String, conversion: String) -> [Segment] {
        let whole = [Segment(reading: reading, conversion: conversion)]
        guard !reading.isEmpty, !conversion.isEmpty else { return whole }

        // 変換結果を「ひらがな連続」と「それ以外（漢字・カタカナ・英数）」のランに分ける
        let runs = splitRuns(conversion)
        // 全部ひらがな（=無変換）なら分割しない
        guard runs.contains(where: { !$0.isKana }) else { return whole }

        // ひらがなランをリテラル、それ以外を最短マッチのグループにした正規表現で読みを照合する
        var pattern = "^"
        for run in runs {
            if run.isKana {
                pattern += NSRegularExpression.escapedPattern(for: run.text)
            } else {
                // カタカナは読みではひらがなで現れるため、判明している場合はリテラルにする
                let hiragana = katakanaToHiragana(run.text)
                if hiragana != run.text && hiragana.allSatisfy({ isHiraganaLike($0) }) {
                    pattern += NSRegularExpression.escapedPattern(for: hiragana)
                } else {
                    pattern += "(.+?)"
                }
            }
        }
        pattern += "$"

        guard let regex = try? NSRegularExpression(pattern: pattern),
              let match = regex.firstMatch(
                in: reading, range: NSRange(reading.startIndex..., in: reading))
        else { return whole }

        // マッチ結果から各ランの読み区間を割り当てる
        var groupIndex = 1
        var pieces: [(readingPart: String, conversionPart: String, isKana: Bool)] = []
        for run in runs {
            if run.isKana {
                pieces.append((run.text, run.text, true))
            } else {
                let hiragana = katakanaToHiragana(run.text)
                if hiragana != run.text && hiragana.allSatisfy({ isHiraganaLike($0) }) {
                    pieces.append((hiragana, run.text, false))
                } else {
                    guard groupIndex <= match.numberOfRanges - 1,
                          let range = Range(match.range(at: groupIndex), in: reading) else { return whole }
                    pieces.append((String(reading[range]), run.text, false))
                    groupIndex += 1
                }
            }
        }

        // 文節を構成: 「非かなラン + 直後のかなラン」を1文節にまとめる。
        // 先頭のかなランは独立した文節にする
        var segments: [Segment] = []
        var index = 0
        while index < pieces.count {
            let piece = pieces[index]
            if piece.isKana {
                segments.append(Segment(reading: piece.readingPart, conversion: piece.conversionPart))
                index += 1
            } else {
                var readingPart = piece.readingPart
                var conversionPart = piece.conversionPart
                if index + 1 < pieces.count, pieces[index + 1].isKana {
                    readingPart += pieces[index + 1].readingPart
                    conversionPart += pieces[index + 1].conversionPart
                    index += 2
                } else {
                    index += 1
                }
                segments.append(Segment(reading: readingPart, conversion: conversionPart))
            }
        }
        return segments.isEmpty ? whole : segments
    }

    // MARK: - 内部

    private struct Run {
        var text: String
        var isKana: Bool
    }

    private static func isHiraganaLike(_ character: Character) -> Bool {
        guard let scalar = character.unicodeScalars.first else { return false }
        // ひらがな + 長音符・繰り返し記号・句読点類は「読みにそのまま現れる」扱い
        return (0x3041...0x309F).contains(scalar.value)
            || "ー、。！？，．・「」".contains(character)
    }

    private static func splitRuns(_ text: String) -> [Run] {
        var runs: [Run] = []
        for character in text {
            // 長音符は直前のラン（カタカナ語の途中など）に追随させる
            if character == "ー", var last = runs.last {
                last.text.append(character)
                runs[runs.count - 1] = last
                continue
            }
            let isKana = isHiraganaLike(character)
            if var last = runs.last, last.isKana == isKana {
                last.text.append(character)
                runs[runs.count - 1] = last
            } else {
                runs.append(Run(text: String(character), isKana: isKana))
            }
        }
        return runs
    }
}
