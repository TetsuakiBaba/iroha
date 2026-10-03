import Foundation

/// ローマ字入力のつもりで英語を打った（英数モードへの切り替え忘れ）可能性がある入力を見分け、
/// 打鍵どおりの英字を候補ウィンドウに出すかを決める。自動では切り替えない（候補に足すだけ）。
///
/// 手がかりは2つ:
/// - **かなにならずに英字が残る**（「test」→「てst」、「computer」→「こmぷてr」）。
///   ローマ字の規則で読めない綴りなので、英語か打ち間違いのどちらか
/// - **英単語の辞書に載っている**（`isEnglishWord`、macOS のスペルチェッカーを渡す）。
///   ただし「kore」「sore」「mono」「made」のようなローマ字も英単語として通るので、
///   これだけでは強い手がかりにしない
///
/// 英字が残るかは、今の読みではなく打鍵からローマ字の規則で作り直した読みで見る。
/// 打ち間違いの訂正が休止で読みを書き換える（「こmぷてr」→「こもうてる」）ので、今の読みには
/// 英字が残っていないことがある
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

    /// 候補にしなかった理由（デバッグ表示に出す）
    public enum Rejection: Sendable, Equatable {
        /// 英字・数字・「'」「-」以外を含む、英字を含まない、または2文字未満
        case notWordShaped
        /// ローマ字としてすべて読め、英単語としても通らない
        case romaji
    }

    /// 判定の結果
    public enum Outcome: Sendable, Equatable {
        case candidate(Candidate)
        case rejected(Rejection)
    }

    /// 打鍵どおりの英字を候補にするか。
    /// - Parameters:
    ///   - raw: 打鍵どおりの文字列（`RomajiComposer.raw`。`rawCoversInput` が偽なら呼ばない）
    ///   - isEnglishWord: 英単語として通るか（英字・数字・「'」「-」だけの語にだけ呼ぶ）
    public static func detect(
        raw: String, isEnglishWord: (String) -> Bool
    ) -> Outcome {
        guard raw.count >= 2, raw.contains(where: \.isASCIILetter),
              raw.allSatisfy({ $0.isASCIILetter || $0.isASCIIDigit || $0 == "'" || $0 == "-" })
        else { return .rejected(.notWordShaped) }
        var composer = RomajiComposer()
        composer.input(raw)
        composer.flush()
        let leftover = composer.text.contains(where: \.isASCIILetter)
        let isWord = isEnglishWord(raw)
        switch (leftover, isWord) {
        case (true, true): return .candidate(Candidate(word: raw, placement: .near))
        case (true, false), (false, true): return .candidate(Candidate(word: raw, placement: .late))
        case (false, false): return .rejected(.romaji)
        }
    }
}

private extension Character {
    var isASCIILetter: Bool { isASCII && isLetter }
    var isASCIIDigit: Bool { isASCII && isNumber }
}
