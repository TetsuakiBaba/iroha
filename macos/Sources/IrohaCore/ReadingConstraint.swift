import Foundation

/// 生成中の出力が入力読みと辻褄が合っているかを追跡する制約（constrained decoding）。
///
/// zenzは自由生成なので、読みに存在しない「。」や「、」を勝手に挿入したり、
/// 読みを食い残したまま終わったりする（例: こんにちはあかちゃん → こんにちは。赤ちゃん）。
/// azooKeyは辞書ラティスで候補を縛ることでこれを防いでいる。irohaには辞書がないため、
/// 「出力の各文字が読みの何文字を消費したか」だけを追う軽量な制約で代用する。
///
/// - ひらがな・句読点・記号は読みにそのまま現れるはずなので、その位置と一致しなければ不許可
/// - 漢字・カタカナ・英数（読みが不定の文字）は読みを1〜maxSpan文字消費したものとみなす
/// - 終端は読みを使い切ったときのみ許可する
///
/// 消費位置は「ありうる位置の集合」をビットマスクで持つ（bit p = 読みをp文字消費した状態）。
struct ReadingConstraint {

    /// 漢字1文字が持ちうる読みの最大長（承る=うけたまわ など）
    private static let maxSpan = 8

    /// ビットマスクに収まる読みの最大長。これを超える読みでは制約をかけない
    static let maxReadingLength = 62

    private let readingLength: Int
    /// 文字 → その文字が読みのどの位置に現れるか（bit p = reading[p]がその文字）
    private let literalPositions: [Character: UInt64]

    /// 読みが空、または長すぎて追跡できない場合はnil（制約なしで生成する）
    init?(reading: String) {
        let characters = Array(katakanaToHiragana(reading))
        guard !characters.isEmpty, characters.count <= Self.maxReadingLength else { return nil }
        readingLength = characters.count
        var positions: [Character: UInt64] = [:]
        for (index, character) in characters.enumerated() {
            positions[character, default: 0] |= UInt64(1) << UInt64(index)
        }
        literalPositions = positions
    }

    /// 何も消費していない初期状態
    var initialMask: UInt64 { 1 }

    /// 読みを使い切った状態を含むか（終端を許可してよいか）
    func isComplete(_ mask: UInt64) -> Bool {
        mask & (UInt64(1) << UInt64(readingLength)) != 0
    }

    /// 出力文字列を1つ消費した後の状態。0なら制約違反（ありうる位置がない）
    func advance(_ mask: UInt64, text: String) -> UInt64 {
        var current = mask
        for character in text {
            current = advance(current, character: character)
            if current == 0 { return 0 }
        }
        return current
    }

    func advance(_ mask: UInt64, character: Character) -> UInt64 {
        let normalized = Character(katakanaToHiragana(String(character)))
        // 読みの同じ文字に重なる位置は1文字進める
        var next = (mask & (literalPositions[normalized] ?? 0)) << 1
        let span = Self.span(of: character)
        if span != .literal {
            // 読みが不定の文字は1〜maxSpan文字を消費したとみなす
            var spread = mask
            for _ in 0..<Self.maxSpan {
                spread <<= 1
                next |= spread
            }
            // 英数字とカタカナは読みを消費しないこともある（WOWOW←わうわう、コンピューター←こんぴゅーた）
            if span == .zeroOrMore { next |= mask }
        }
        // 読みの長さを超えた位置は捨てる
        let overflow = UInt64.max << UInt64(readingLength + 1)
        return next & ~overflow
    }

    /// 1文字が読みを何文字消費しうるか
    enum Span: Equatable {
        /// 読みにそのまま現れるはず（ひらがな・句読点・記号）
        case literal
        /// 読みは不定だが必ず1文字以上消費する（漢字・々）
        case oneOrMore
        /// 読みを消費しないこともある（カタカナ・英数字。長音や頭字語で字数が合わない）
        case zeroOrMore
    }

    static func span(of character: Character) -> Span {
        guard let scalar = character.unicodeScalars.first else { return .literal }
        switch scalar.value {
        case 0x3041...0x309F:            // ひらがな（濁点・繰り返し記号を含む）
            return .literal
        case 0x3005:                     // 々（読みは直前の漢字次第）
            return .oneOrMore
        case 0x3000...0x303F:            // 、。「」・… などの和文記号
            return .literal
        case 0x20...0x2F, 0x3A...0x40, 0x5B...0x60, 0x7B...0x7E:  // ASCII記号・空白
            return .literal
        case 0xFF01...0xFF0F, 0xFF1A...0xFF20, 0xFF3B...0xFF40, 0xFF5B...0xFF65:  // 全角記号
            return .literal
        case 0x30A0...0x30FF, 0xFF66...0xFF9F:                    // カタカナ（長音符を含む）
            return .zeroOrMore
        case 0x30...0x39, 0x41...0x5A, 0x61...0x7A:               // ASCII英数字
            return .zeroOrMore
        case 0xFF10...0xFF19, 0xFF21...0xFF3A, 0xFF41...0xFF5A:   // 全角英数字
            return .zeroOrMore
        default:                         // 漢字など
            return .oneOrMore
        }
    }
}
