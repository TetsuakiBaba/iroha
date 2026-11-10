import Foundation

/// 音声認識の結果（漢字かな交じりの表記）を、かな漢字変換に通す「読み」と、読みに戻さない「固定部分」に分けたもの
/// （「こえ」モード。2026-11-10）。
///
/// 音声認識は表記を出すので、分かち書きして読みを付け（`ReadingTokenizer`。macOS では `SystemReadingTokenizer`）、
/// 読みが付いた語はつなげて読みにし、付かなかった語（英字・数字・記号、読みを求められなかった漢字）は
/// 認識結果の表記のまま固定部分にする。読みは打鍵で入れた読みと同じ形（ひらがなと、ローマ字入力で打てる記号）にする。
///
/// 読みの部分には認識結果の表記（`surface`）も残す。読みの復元は間違えることがあるので
/// （「今日」= きょう／こんにち、人名）、表記を候補として出せるようにするため
public struct VoiceTranscript: Sendable, Equatable {

    public enum Part: Sendable, Equatable {
        /// かな漢字変換に通す読み。`surface` は音声認識が出した表記
        case reading(String, surface: String)
        /// 読みに戻さない部分。認識結果の表記のまま入れる
        case fixed(String)
    }

    public var parts: [Part]

    public init(parts: [Part]) {
        self.parts = parts
    }

    /// - Parameters:
    ///   - tokens: 認識結果を分かち書きして読みを付けたもの（つなげると認識結果に戻る）
    ///   - comma: 読点として読みに入れる文字（句読点スタイル。「、」か「，」）
    ///   - period: 句点として読みに入れる文字（「。」か「．」）
    public init(tokens: [ReadingToken], comma: String = "、", period: String = "。") {
        var parts: [Part] = []
        // 固定部分の直後の空白。英字・数字どうしの間なら 1 つ残し（英単語の間）、それ以外は捨てる
        // （SpeechTranscriber は英字・数字の前に空白を入れる。「東京で iPhoneの」「2026年 11月」）
        var pendingSpace = false

        func appendReading(_ reading: String, surface: String) {
            pendingSpace = false
            if case .reading(let previous, let previousSurface)? = parts.last {
                parts[parts.count - 1] = .reading(previous + reading, surface: previousSurface + surface)
            } else {
                parts.append(.reading(reading, surface: surface))
            }
        }
        func appendFixed(_ text: String) {
            if case .fixed(let previous)? = parts.last {
                let keepsSpace = pendingSpace && previous.last?.isASCII == true && text.first?.isASCII == true
                parts[parts.count - 1] = .fixed(previous + (keepsSpace ? " " : "") + text)
            } else {
                parts.append(.fixed(text))
            }
            pendingSpace = false
        }

        for token in tokens {
            // 数字の直後の漢字だけの語（年・月・日・人・時間などの助数詞）は、表記のまま数字とつなげる。
            // 数字と切り離して読みを求めると助数詞の読みを取り違える（「10日」の日 → か）。
            // 数字つきの表記は音声認識の出したままで足りる
            if !pendingSpace, case .fixed(let previous)? = parts.last, previous.last?.isNumber == true,
               previous.last?.unicodeScalars.contains(where: DocumentText.isKanji) == false,
               !token.surface.isEmpty, token.surface.unicodeScalars.allSatisfy(DocumentText.isKanji) {
                parts[parts.count - 1] = .fixed(previous + token.surface)
                continue
            }
            if let reading = token.reading, !reading.isEmpty {
                let styled = reading.replacingOccurrences(of: "、", with: comma)
                    .replacingOccurrences(of: "。", with: period)
                appendReading(styled, surface: token.surface)
                continue
            }
            // 読みが付かなかった語は 1 文字ずつ見る（「？」のように打鍵で読みに入る記号と、空白を分ける）
            var fixed = ""
            func flushFixed() {
                guard !fixed.isEmpty else { return }
                appendFixed(fixed)
                fixed = ""
            }
            for character in token.surface {
                if character.isWhitespace {
                    flushFixed()
                    if case .fixed? = parts.last { pendingSpace = true }
                } else if let symbol = Self.readingSymbols[character] {
                    flushFixed()
                    appendReading(symbol, surface: String(character))
                } else {
                    fixed.append(character)
                }
            }
            flushFixed()
        }
        self.parts = parts
    }

    /// ローマ字入力で読みに入る記号（`RomajiComposer` の表と同じ文字にそろえる）。
    /// 読みに入れておくと、かな漢字変換モデルが打鍵のときと同じ形の入力を受け取る
    static let readingSymbols: [Character: String] = [
        "？": "？", "?": "？", "！": "！", "!": "！",
        "「": "「", "」": "」", "・": "・", "ー": "ー", "〜": "〜",
    ]

    public var isEmpty: Bool { parts.isEmpty }

    /// 音声認識の表記（空白を除いたもの）
    public var surface: String {
        parts.map { part in
            switch part {
            case .reading(_, let surface): return surface
            case .fixed(let text): return text
            }
        }.joined()
    }

    /// 読みと固定部分を順につなげたもの（確認用の表示）
    public var readingText: String {
        parts.map { part in
            switch part {
            case .reading(let reading, _): return reading
            case .fixed(let text): return text
            }
        }.joined()
    }
}
