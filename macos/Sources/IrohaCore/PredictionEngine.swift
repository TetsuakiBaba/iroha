import Foundation

/// 次に続く文字列を予測するエンジンの抽象（予測変換・インライン補完）。
///
/// かな漢字変換（`ConversionEngine`）とは別のモデルを使えるように分けている。
/// 現行実装: `ZenzEngine`（zenz-v3で左文脈の続きを生成する）
public protocol PredictionEngine: Sendable {
    /// 左文脈に続く文字列を予測する。
    /// - Parameters:
    ///   - context: 左文脈（確定済みの文字列や、入力中の変換結果まで）
    ///   - maxLength: 予測の最大文字数（これ以上は生成しない）
    /// - Returns: 続きとして表示する文字列（1文節程度、句読点が出たらそこまで）。予測できなければ空
    func predict(context: String, maxLength: Int) async throws -> String

    /// モデルの事前ロード。必要のない実装は何もしなくてよい
    func prewarm() async throws
}

extension PredictionEngine {
    public func prewarm() async throws {}
}

/// モデルが生成した続きから、予測として表示する範囲（先頭の1文節）を切り出す。
///
/// 生成はトークン単位で進むので、1トークン足すごとに呼び、`isComplete` が真になったら
/// 生成を止める（それ以上生成しても表示は変わらない）。
///
/// - 句読点（、。！？，．）が出たら、その句読点までで打ち切る
/// - 改行・空白・特殊トークンが出たら、その手前までで打ち切る
/// - 文節境界は「かなの連なりの直後に漢字・カタカナ・英数が始まる位置」とみなす
///   （`ReadingAligner` と同じ考え方）。1文節目が短すぎる（2文字未満）ときは2文節目まで含める
/// - 境界が見つからないまま `maxLength` に達したら、そこで打ち切る
public enum PredictionText {

    /// 予測を打ち切る句読点
    public static let punctuation: Set<Character> = ["、", "。", "！", "？", "，", "．"]

    /// 1文節がこれより短ければ次の文節まで含める
    static let minimumPhraseLength = 2

    public struct Phrase: Equatable, Sendable {
        public var text: String
        /// これ以上生成しても `text` が変わらないか
        public var isComplete: Bool
    }

    public static func phrase(in generated: String, maxLength: Int) -> Phrase {
        var characters: [Character] = []
        for character in generated {
            // 改行・空白・制御文字・書式文字（異体字セレクタ等）・zenzの特殊トークン（私用領域）は
            // 続きとして扱わない。文字に結合した見えないスカラ（「い」+ 異体字セレクタ）は落として文字だけ残す
            let visibleScalars = character.unicodeScalars.filter(isVisible)
            guard !character.isNewline, !character.isWhitespace, let first = visibleScalars.first,
                  first == character.unicodeScalars.first else {
                return Phrase(text: String(characters), isComplete: true)
            }
            characters.append(Character(String(String.UnicodeScalarView(visibleScalars))))
            if punctuation.contains(character) {
                return Phrase(text: String(characters), isComplete: true)
            }
            if characters.count >= maxLength {
                return Phrase(text: String(characters.prefix(maxLength)), isComplete: true)
            }
        }

        // 文節境界（かな → 非かな）を探す
        let boundaries = bunsetsuBoundaries(characters)
        guard let first = boundaries.first else {
            return Phrase(text: String(characters), isComplete: false)
        }
        if first >= minimumPhraseLength {
            return Phrase(text: String(characters[..<first]), isComplete: true)
        }
        // 1文節目が短い（助詞1文字など）: 2文節目の終わりまで待つ
        if boundaries.count >= 2 {
            return Phrase(text: String(characters[..<boundaries[1]]), isComplete: true)
        }
        return Phrase(text: String(characters), isComplete: false)
    }

    /// かなの連なりが終わって非かなが始まる位置（先頭は含まない）。
    /// 長音符「ー」は直前の文字の区分に従う（カタカナ語の途中で切らない）
    private static func bunsetsuBoundaries(_ characters: [Character]) -> [Int] {
        var isKana: [Bool] = []
        for (index, character) in characters.enumerated() {
            if character == "ー", index > 0 {
                isKana.append(isKana[index - 1])
            } else {
                isKana.append(isHiragana(character))
            }
        }
        var boundaries: [Int] = []
        for index in 1..<max(characters.count, 1) where isKana[index - 1] && !isKana[index] {
            boundaries.append(index)
        }
        return boundaries
    }

    /// 目に見えるスカラか（制御・書式・結合記号・私用領域・未割り当ては予測に含めない）
    static func isVisible(_ scalar: Unicode.Scalar) -> Bool {
        switch scalar.properties.generalCategory {
        case .control, .format, .privateUse, .unassigned, .surrogate,
             .nonspacingMark, .enclosingMark, .lineSeparator, .paragraphSeparator:
            return false
        default:
            return true
        }
    }

    /// ひらがな。ReadingAlignerの「読みにそのまま現れる文字」と同じ区分
    static func isHiragana(_ character: Character) -> Bool {
        guard let scalar = character.unicodeScalars.first else { return false }
        return (0x3041...0x309F).contains(scalar.value)
    }
}
