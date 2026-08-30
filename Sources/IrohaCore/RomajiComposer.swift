import Foundation

/// ローマ字入力を逐次ひらがなに変換する合成器。
/// MS-IME/mozc互換の規則:
///   - 「nn」「n'」→「ん」、「n」+子音 →「ん」+子音
///   - 同一子音の連続 →「っ」（tch も「っ」+ ch）
///   - 対応しない文字はそのまま通す
public struct RomajiComposer: Sendable, Equatable {
    /// 確定済みのかな文字列
    public private(set) var text: String = ""
    /// 未解決のローマ字（例: "ky" の状態）
    public private(set) var pending: String = ""
    /// 打鍵されたままの文字列（F9/F10の英数変換用）
    public private(set) var raw: String = ""
    /// rawが表示内容と対応しているか（かな部分を削除するとずれるためfalseになる）
    public private(set) var rawIsReliable: Bool = true

    /// 変換テーブル（句読点スタイルを反映したもの）
    private let table: [String: String]

    /// - Parameters:
    ///   - commaText: 「,」キーで入力する文字（"、" または "，"）
    ///   - periodText: 「.」キーで入力する文字（"。" または "．"）
    public init(commaText: String = "、", periodText: String = "。") {
        var table = Self.defaultTable
        table[","] = commaText
        table["."] = periodText
        self.table = table
    }

    /// 画面表示用の未確定文字列（かな + 未解決ローマ字）
    public var display: String { text + pending }

    public var isEmpty: Bool { text.isEmpty && pending.isEmpty }

    public mutating func input(_ character: Character) {
        raw.append(character)
        // 英字は小文字に正規化して扱う
        let c = Character(character.lowercased())
        pending.append(c)
        resolve()
    }

    public mutating func input(_ string: String) {
        for c in string { input(c) }
    }

    /// 表示上の末尾1文字を削除する
    public mutating func deleteBackward() {
        if !pending.isEmpty {
            pending.removeLast()
            // 未解決ローマ字の削除はrawと1対1で対応する
            if !raw.isEmpty { raw.removeLast() }
        } else if !text.isEmpty {
            text.removeLast()
            // かな1文字は複数打鍵に対応するためrawとの対応が崩れる
            rawIsReliable = false
        }
    }

    /// 未解決の末尾を確定する（"n" → "ん"、それ以外はそのまま残す）
    public mutating func flush() {
        while !pending.isEmpty {
            if let value = self.table[pending] {
                text += value
                pending = ""
            } else {
                // 先頭から可能な限り変換し、残りは文字として確定
                let before = pending
                forceResolveHead()
                if pending == before {
                    text.append(pending.removeFirst())
                }
            }
        }
    }

    public mutating func clear() {
        text = ""
        pending = ""
        raw = ""
        rawIsReliable = true
    }

    // MARK: - 変換規則

    private mutating func resolve() {
        while !pending.isEmpty {
            let canExtend = Self.prefixes.contains(pending)
            if let exact = self.table[pending], !canExtend {
                text += exact
                pending = ""
                continue
            }
            if canExtend {
                return  // 次の入力を待つ（例: "k", "n", "ch"）
            }
            forceResolveHead()
        }
    }

    /// pending がどのキーの接頭辞でもないときに先頭部分を強制的に解決する
    private mutating func forceResolveHead() {
        // 最長一致する完全なキーを探す（例: "nk" → "n"=ん を出して "k" を残す）
        for length in stride(from: pending.count - 1, through: 1, by: -1) {
            let prefix = String(pending.prefix(length))
            if let value = self.table[prefix] {
                text += value
                pending.removeFirst(length)
                return
            }
        }
        // 促音: 同一子音の連続、または tch
        if pending.count >= 2 {
            let c0 = pending[pending.startIndex]
            let c1 = pending[pending.index(after: pending.startIndex)]
            if (c0 == c1 && Self.sokuonConsonants.contains(c0)) || (c0 == "t" && c1 == "c") {
                text += "っ"
                pending.removeFirst()
                return
            }
        }
        // 「n」+子音（および単独の n の確定）→「ん」
        if pending.first == "n" {
            text += "ん"
            pending.removeFirst()
            return
        }
        // 変換不能な文字はそのまま出力
        text.append(pending.removeFirst())
    }

    private static let sokuonConsonants = Set("bcdfghjklmpqrstvwxyz")

    static let defaultTable: [String: String] = [
        // 母音
        "a": "あ", "i": "い", "u": "う", "e": "え", "o": "お",
        // か行
        "ka": "か", "ki": "き", "ku": "く", "ke": "け", "ko": "こ",
        "kya": "きゃ", "kyi": "きぃ", "kyu": "きゅ", "kye": "きぇ", "kyo": "きょ",
        "ga": "が", "gi": "ぎ", "gu": "ぐ", "ge": "げ", "go": "ご",
        "gya": "ぎゃ", "gyu": "ぎゅ", "gyo": "ぎょ",
        // さ行
        "sa": "さ", "si": "し", "su": "す", "se": "せ", "so": "そ",
        "sha": "しゃ", "shi": "し", "shu": "しゅ", "she": "しぇ", "sho": "しょ",
        "sya": "しゃ", "syi": "しぃ", "syu": "しゅ", "sye": "しぇ", "syo": "しょ",
        "za": "ざ", "zi": "じ", "zu": "ず", "ze": "ぜ", "zo": "ぞ",
        "ja": "じゃ", "ji": "じ", "ju": "じゅ", "je": "じぇ", "jo": "じょ",
        "jya": "じゃ", "jyu": "じゅ", "jyo": "じょ",
        "zya": "じゃ", "zyi": "じぃ", "zyu": "じゅ", "zye": "じぇ", "zyo": "じょ",
        // た行
        "ta": "た", "ti": "ち", "tu": "つ", "te": "て", "to": "と",
        "cha": "ちゃ", "chi": "ち", "chu": "ちゅ", "che": "ちぇ", "cho": "ちょ",
        "tya": "ちゃ", "tyi": "ちぃ", "tyu": "ちゅ", "tye": "ちぇ", "tyo": "ちょ",
        "tsu": "つ", "tsa": "つぁ", "tsi": "つぃ", "tse": "つぇ", "tso": "つぉ",
        "tha": "てゃ", "thi": "てぃ", "thu": "てゅ", "the": "てぇ", "tho": "てょ",
        "twu": "とぅ",
        "da": "だ", "di": "ぢ", "du": "づ", "de": "で", "do": "ど",
        "dya": "ぢゃ", "dyu": "ぢゅ", "dyo": "ぢょ",
        "dha": "でゃ", "dhi": "でぃ", "dhu": "でゅ", "dhe": "でぇ", "dho": "でょ",
        "dwu": "どぅ",
        // な行
        "na": "な", "ni": "に", "nu": "ぬ", "ne": "ね", "no": "の",
        "nya": "にゃ", "nyi": "にぃ", "nyu": "にゅ", "nye": "にぇ", "nyo": "にょ",
        "nn": "ん", "n'": "ん",
        // は行
        "ha": "は", "hi": "ひ", "hu": "ふ", "he": "へ", "ho": "ほ",
        "hya": "ひゃ", "hyu": "ひゅ", "hyo": "ひょ",
        "fa": "ふぁ", "fi": "ふぃ", "fu": "ふ", "fe": "ふぇ", "fo": "ふぉ",
        "fya": "ふゃ", "fyu": "ふゅ", "fyo": "ふょ",
        "ba": "ば", "bi": "び", "bu": "ぶ", "be": "べ", "bo": "ぼ",
        "bya": "びゃ", "byu": "びゅ", "byo": "びょ",
        "pa": "ぱ", "pi": "ぴ", "pu": "ぷ", "pe": "ぺ", "po": "ぽ",
        "pya": "ぴゃ", "pyu": "ぴゅ", "pyo": "ぴょ",
        // ま行
        "ma": "ま", "mi": "み", "mu": "む", "me": "め", "mo": "も",
        "mya": "みゃ", "myu": "みゅ", "myo": "みょ",
        // や行
        "ya": "や", "yu": "ゆ", "yo": "よ", "ye": "いぇ",
        // ら行
        "ra": "ら", "ri": "り", "ru": "る", "re": "れ", "ro": "ろ",
        "rya": "りゃ", "ryu": "りゅ", "ryo": "りょ",
        // わ行
        "wa": "わ", "wi": "うぃ", "wu": "う", "we": "うぇ", "wo": "を",
        "wha": "うぁ", "whi": "うぃ", "whe": "うぇ", "who": "うぉ",
        // ゔ
        "va": "ゔぁ", "vi": "ゔぃ", "vu": "ゔ", "ve": "ゔぇ", "vo": "ゔぉ",
        // c/q 系（Google日本語入力互換）
        "ca": "か", "ci": "し", "cu": "く", "ce": "せ", "co": "こ",
        "qa": "くぁ", "qi": "くぃ", "qu": "く", "qe": "くぇ", "qo": "くぉ",
        // 小書き
        "la": "ぁ", "li": "ぃ", "lu": "ぅ", "le": "ぇ", "lo": "ぉ",
        "xa": "ぁ", "xi": "ぃ", "xu": "ぅ", "xe": "ぇ", "xo": "ぉ",
        "ltu": "っ", "xtu": "っ", "ltsu": "っ",
        "lya": "ゃ", "lyu": "ゅ", "lyo": "ょ",
        "xya": "ゃ", "xyu": "ゅ", "xyo": "ょ",
        "lwa": "ゎ", "xwa": "ゎ",
        "xka": "ヵ", "xke": "ヶ",
        // 記号
        "-": "ー", ",": "、", ".": "。", "/": "・",
        "[": "「", "]": "」", "!": "！", "?": "？", "~": "〜",
    ]

    /// 全キーの真の接頭辞集合（次の入力を待つべきか判定に使う）
    static let prefixes: Set<String> = {
        var set = Set<String>()
        for key in defaultTable.keys {
            var prefix = key
            while prefix.count > 1 {
                prefix.removeLast()
                set.insert(prefix)
            }
        }
        // 「n」+子音 → 「ん」を機能させるため "n" 自体は完全キーにしない代わりに
        // 接頭辞として待つ（na, ni, ... nn, n' の接頭辞なので自動的に含まれる）
        return set
    }()
}
