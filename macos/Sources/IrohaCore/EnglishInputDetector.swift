import Foundation

/// ローマ字入力のつもりで英語を打った（英数モードへの切り替え忘れ）可能性がある入力を見分け、
/// 打鍵どおりの英字を候補ウィンドウに出すかを決める。自動では切り替えない（候補に足すだけ）。
///
/// 手がかりは2つ:
/// - **かなにならずに英字が残った**（「test」→「てst」、「computer」→「こmぷてr」）。
///   ローマ字の規則で読めない綴りなので、英語か打ち間違いのどちらか
/// - **英単語の辞書に載っている**（`isEnglishWord`、macOS のスペルチェッカーを渡す）。
///   ただし「kore」「sore」「mono」「made」のようなローマ字も英単語として通るので、
///   これだけでは強い手がかりにしない
public enum EnglishInputDetector {

    /// 候補ウィンドウのどこに置くか
    public enum Placement: Sendable, Equatable {
        /// 第一候補のすぐ後ろ（英字が残り、かつ英単語として通る）
        case near
        /// かな・カタカナの定番候補の手前（どちらか一方の手がかりしかない）
        case late
    }

    public struct Candidate: Sendable, Equatable {
        /// 打鍵どおりの英字
        public var word: String
        public var placement: Placement
    }

    /// 打鍵どおりの英字を候補にするか。
    /// - Parameters:
    ///   - raw: 打鍵どおりの文字列（`RomajiComposer.raw`。`rawIsReliable` が偽なら呼ばない）
    ///   - reading: 未解決のローマ字を確定したあとの読み（`RomajiComposer.flush()` 後の `text`）
    ///   - isEnglishWord: 英単語として通るか（英字・数字・「'」「-」だけの語にだけ呼ぶ）
    public static func candidate(
        raw: String, reading: String, isEnglishWord: (String) -> Bool
    ) -> Candidate? {
        guard raw.count >= 2, raw.contains(where: \.isASCIILetter),
              raw.allSatisfy({ $0.isASCIILetter || $0.isASCIIDigit || $0 == "'" || $0 == "-" })
        else { return nil }
        let leftover = reading.contains(where: \.isASCIILetter)
        let isWord = isEnglishWord(raw)
        switch (leftover, isWord) {
        case (true, true): return Candidate(word: raw, placement: .near)
        case (true, false), (false, true): return Candidate(word: raw, placement: .late)
        case (false, false): return nil
        }
    }
}

private extension Character {
    var isASCIILetter: Bool { isASCII && isLetter }
    var isASCIIDigit: Bool { isASCII && isNumber }
}
