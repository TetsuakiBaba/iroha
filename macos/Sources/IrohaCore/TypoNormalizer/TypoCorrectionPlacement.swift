import Foundation

/// 読み全体に対する訂正を、文節ごとの候補に落とすための計算。
///
/// モデルは**読み全体**を見て訂正する（学習データが文・節の単位なので、文節の断片を
/// 入れると「打ちかけの読み」と同じ扱いになって当たらない）。一方 iroha の候補ウィンドウは
/// 文節ごとに開く。そこで「読みのどこが書き換わったか」を出し、その差分が丸ごと収まっている
/// 文節にだけ訂正候補を足す。
///
/// 差分が文節境界をまたぐときは何も出さない。またぐケースでは訂正を出すのに文節の切り直しが
/// 要り、ライブ変換の結果を黙って作り替えることになるため（SWIFT-PORT.md §5 の「読みを
/// 黙って書き換えるのではなく候補ウィンドウに合流させる」に反する）
extension TypoCorrection {

    /// 訂正が読みのどこを書き換えるか。`range` は元の読みの文字インデックス
    /// （前後の一致部分を取り除いた最小の差分）、`replacement` はそこへ入る文字列
    public var edit: (range: Range<Int>, replacement: String) {
        let original = Array(reading)
        let fixed = Array(corrected)
        var prefix = 0
        while prefix < original.count, prefix < fixed.count, original[prefix] == fixed[prefix] {
            prefix += 1
        }
        var suffix = 0
        while suffix < original.count - prefix, suffix < fixed.count - prefix,
              original[original.count - 1 - suffix] == fixed[fixed.count - 1 - suffix] {
            suffix += 1
        }
        return (prefix..<(original.count - suffix),
                String(fixed[prefix..<(fixed.count - suffix)]))
    }

    /// 訂正が「読みの末尾に文字を足しただけ」か。
    ///
    /// 合成中（まだ打ち終わっていない読み）に対しては、これは訂正ではなく**続きの補完**で、
    /// ほぼ必ず誤りになる。モデルの学習データが句読点で終わる節なので、
    /// 「こえて」→「こえて、」「やはり」→「やはり。」のように末尾へ句読点を足したがる。
    /// 打ちかけの読みは常に「終わりが足りない」ように見えるので、
    /// 入力中の訂正ではこれを捨てること（実測: 文節境界での誤検出の大半がこれ）
    public var isTrailingInsertionOnly: Bool {
        let (range, replacement) = edit
        return range.isEmpty && range.lowerBound == reading.count && !replacement.isEmpty
    }

    /// 元の読みの `start..<end`（1文節ぶん）に差分が収まっていれば、その文節の訂正後の読み。
    /// またいでいる・関係ない文節なら nil
    public func correctedSegment(start: Int, end: Int) -> String? {
        let original = Array(reading)
        guard start >= 0, end <= original.count, start < end else { return nil }
        let (range, replacement) = edit
        guard range.lowerBound >= start, range.upperBound <= end else { return nil }
        let result = String(original[start..<range.lowerBound]) + replacement
            + String(original[range.upperBound..<end])
        return result == String(original[start..<end]) ? nil : result
    }

    /// 文節の読みを並び順に渡して、訂正が当たる文節の番号と訂正後の読みを返す。
    /// 当たる文節が無ければ nil（差分が境界をまたいだ場合を含む）
    public func placement(inSegments readings: [String]) -> (index: Int, correctedReading: String)? {
        guard readings.joined() == reading else { return nil }
        var start = 0
        for (index, segment) in readings.enumerated() {
            let end = start + segment.count
            if let corrected = correctedSegment(start: start, end: end) {
                return (index, corrected)
            }
            start = end
        }
        return nil
    }

    /// 文節の列のうち、訂正の対象になった読みが始まる位置（先頭の固定部分の数）。
    /// 一致する切れ目が無ければ nil
    public func segmentOffset(inSegments readings: [String]) -> Int? {
        for offset in readings.indices where readings[offset...].joined() == reading { return offset }
        return readings.joined() == reading ? 0 : nil
    }

    /// 先頭に訂正の対象外だった文節（英字の固定部分・取り入れた予測など）が付いていてもよい版。
    /// 末尾から順に読みをつないでいって、訂正に渡した読みと一致するところを対象にする。
    /// 返す番号は `readings` 全体での番号
    public func placement(inSegmentsWithFixedPrefix readings: [String])
        -> (index: Int, correctedReading: String)?
    {
        for offset in 0..<max(1, readings.count) where offset < readings.count {
            let tail = Array(readings[offset...])
            guard tail.joined() == reading else { continue }
            guard let placement = placement(inSegments: tail) else { return nil }
            return (placement.index + offset, placement.correctedReading)
        }
        return nil
    }
}
