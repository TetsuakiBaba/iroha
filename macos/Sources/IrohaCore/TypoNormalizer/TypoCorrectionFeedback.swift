import Foundation

/// 「何をどう直したか」をカーソル下の小窓に出すための、読みの割り付け。
///
/// 訂正は**読み**に対して起きるが、ライブ変換がONだと画面に出るのは変換後の漢字かな交じり文で、
/// 読みのどこが直ったかは見えない。未確定文字列に属性を付ける手もあるが、属性の解釈はアプリ任せで
/// （Electron系は無視する）、そもそも読みの位置と変換結果の文字位置は対応しない。
/// そこで読みそのものを「打った読み → 直した読み」の形で別ウィンドウに出す。
///
/// 前後の共通部分は**元と訂正後で同じ文字列**（省略記号込み）にする。片方だけ削ると
/// 2行に並べたときに桁がずれて、どこが変わったのか読み取れなくなる
public struct TypoCorrectionFeedback: Equatable, Sendable {
    /// 変わらなかった前の部分。頭を削ったら先頭に "…" が付く
    public let prefix: String
    /// 打った読みのうち変わった部分（挿入だけの訂正なら空）
    public let originalChanged: String
    /// 直した読みのうち変わった部分（削除だけの訂正なら空）
    public let correctedChanged: String
    /// 変わらなかった後の部分。尻を削ったら末尾に "…" が付く
    public let suffix: String

    public init(prefix: String, originalChanged: String, correctedChanged: String, suffix: String) {
        self.prefix = prefix
        self.originalChanged = originalChanged
        self.correctedChanged = correctedChanged
        self.suffix = suffix
    }

    /// 小窓の左側に出す文字列
    public var original: String { prefix + originalChanged + suffix }
    /// 小窓の右側に出す文字列
    public var corrected: String { prefix + correctedChanged + suffix }

    /// 差分の前後 `context` 文字だけを残した割り付け。差分が無ければ nil（空の窓を出さない）。
    ///
    /// 読みは最大48文字（`TypoNormalizer.maxReadingLength`）まで来るので、そのまま出すと
    /// 小窓が画面幅いっぱいになる。変わった部分も `maxChanged` 文字で切る
    public static func make(
        from correction: TypoCorrection, context: Int = 6, maxChanged: Int = 12
    ) -> TypoCorrectionFeedback? {
        let (range, replacement) = correction.edit
        guard !range.isEmpty || !replacement.isEmpty else { return nil }
        let characters = Array(correction.reading)

        var prefix = String(characters[..<range.lowerBound])
        if prefix.count > context { prefix = "…" + String(prefix.suffix(context)) }
        var suffix = String(characters[range.upperBound...])
        if suffix.count > context { suffix = String(suffix.prefix(context)) + "…" }

        return TypoCorrectionFeedback(
            prefix: prefix,
            originalChanged: abbreviate(String(characters[range]), to: maxChanged),
            correctedChanged: abbreviate(replacement, to: maxChanged),
            suffix: suffix
        )
    }

    private static func abbreviate(_ text: String, to limit: Int) -> String {
        text.count > limit ? String(text.prefix(limit)) + "…" : text
    }
}
