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

    // MARK: - 読みの中の英単語を英字に書き換える（入力の休止で自動に）

    /// 打鍵の中の英単語を英字にした読み
    public struct Rewrite: Sendable, Equatable {
        /// 書き換えた読み（英単語の部分は英字、ほかは打鍵どおりのローマ字をかなにしたもの）
        public var reading: String
        /// 英字にした語（打鍵の順）
        public var words: [String]
    }

    /// 打鍵の中から、ローマ字で読めない英字を含む英単語を探し、読みのその部分を英字にする。
    /// 「kyouhacomputerwo」→「きょうはcomputerを」（打鍵どおりなら「きょうはこmぷてrを」）。
    ///
    /// 英字にするのは、次をすべて満たす打鍵の区間:
    /// - かなにならずに残る英字を含む（「make」→「まけ」のようにローマ字として読める語は対象外）
    /// - 英字・「'」「-」だけで `minimumLength` 文字以上、英単語として通る（`isEnglishWord`）
    /// - 区間の前後で区切ってローマ字をかなにしても、区切らないときと同じかなになる
    ///   （「kyouh|acomputer」のように音節の途中で切らない）。ただし区間の終わりは、同じ子音が
    ///   続くところでも切ってよい（「testto」は続けると「てsっと」になるが「test|と」と読む）
    /// 残る英字ごとに、それを含む区間のうち残る英字を最も多く含むもの（同じなら長いもの）を選ぶ
    /// （「sonotestno」で「notes」ではなく「test」）。`excluding` の語は英字にしない（取り消された語）
    public static func rewrite(
        raw: String, minimumLength: Int = 3, excluding: Set<String> = [],
        isEnglishWord: (String) -> Bool
    ) -> Rewrite? {
        let characters = Array(raw)
        let count = characters.count
        guard count >= minimumLength else { return nil }
        let leftover = leftoverIndices(characters)
        guard !leftover.isEmpty else { return nil }

        let whole = kana(characters[...])
        // 区切ってもかなが変わらない位置
        let boundaries = (0...count).map { index in
            index == 0 || index == count
                || kana(characters[..<index]) + kana(characters[index...]) == whole
        }
        let maximumLength = 24
        // 区間の終わりは、同じ子音が続くところ（続けると「っ」になる）でも切ってよい
        let ends = (0...count).map { index in
            boundaries[index]
                || (index > 0 && index < count && characters[index] == characters[index - 1]
                    && sokuonConsonants.contains(characters[index]))
        }
        var spans: [Range<Int>] = []
        for index in leftover.sorted() where !spans.contains(where: { $0.contains(index) }) {
            var best: (span: Range<Int>, covered: Int)?
            for length in stride(from: min(maximumLength, count), through: minimumLength, by: -1) {
                for start in max(0, index - length + 1)...index {
                    let end = start + length
                    guard end <= count, boundaries[start], ends[end],
                          !spans.contains(where: { $0.overlaps(start..<end) }),
                          characters[start..<end].allSatisfy({ $0.isASCIILetter || $0 == "'" || $0 == "-" })
                    else { continue }
                    // 長い順に見ているので、残る英字の数で上回るときだけ入れ替える
                    let covered = leftover.filter { (start..<end).contains($0) }.count
                    if let best, best.covered >= covered { continue }
                    let word = String(characters[start..<end])
                    guard !excluding.contains(word), isEnglishWord(word) else { continue }
                    best = (start..<end, covered)
                }
            }
            if let best { spans.append(best.span) }
        }
        guard !spans.isEmpty else { return nil }
        spans.sort { $0.lowerBound < $1.lowerBound }
        var reading = ""
        var position = 0
        for span in spans {
            reading += kana(characters[position..<span.lowerBound])
            reading += String(characters[span])
            position = span.upperBound
        }
        reading += kana(characters[position...])
        return Rewrite(reading: reading, words: spans.map { String(characters[$0]) })
    }

    private static let sokuonConsonants = Set("bcdfghjklmpqrstvwxyz")

    /// 打鍵をローマ字の規則でかなにする（未解決の末尾も確定する）
    private static func kana(_ characters: ArraySlice<Character>) -> String {
        var composer = RomajiComposer()
        for character in characters { composer.input(character) }
        composer.flush()
        return composer.text
    }

    /// かなにならずに英字のまま残る打鍵の位置。
    /// 未解決のローマ字（`pending`）は常に打鍵の末尾なので、1打鍵ごとに解決された打鍵の範囲がわかる。
    /// その範囲から、かなに足された英字を順に照らし合わせて位置を決める
    /// （「lwo」は「l」+「を」になる。範囲全体ではなく l だけに印をつける）
    private static func leftoverIndices(_ characters: [Character]) -> Set<Int> {
        var composer = RomajiComposer()
        var result = Set<Int>()
        func step(at index: Int, _ action: (inout RomajiComposer) -> Void) {
            let textBefore = composer.text.count
            let pendingBefore = composer.pending.count
            action(&composer)
            var cursor = index - pendingBefore
            let resolvedEnd = index + (index < characters.count ? 1 : 0) - composer.pending.count
            for letter in composer.text.dropFirst(textBefore) where letter.isASCIILetter {
                while cursor < resolvedEnd, Character(characters[cursor].lowercased()) != letter { cursor += 1 }
                guard cursor < resolvedEnd else { break }
                result.insert(cursor)
                cursor += 1
            }
        }
        for (index, character) in characters.enumerated() {
            step(at: index) { $0.input(character) }
        }
        step(at: characters.count) { $0.flush() }
        return result
    }
}

private extension Character {
    var isASCIILetter: Bool { isASCII && isLetter }
    var isASCIIDigit: Bool { isASCII && isNumber }
}
