import Foundation

/// 「AIで直す」（誤変換の疑いがあるときに AI に直させる。開発者モードの項目・既定OFF。2026-11-07）の規則。
///
/// 疑いの判定は、モデルが変換結果を生成したときの文字ごとのマージン（読み制約を満たす 1 位と 2 位の
/// トークンの対数確率の差）の最小値で行う。測定（`experiments/ai-fallback/README.md`、リポジトリ外）では
/// 文単位の AUROC が AJIMEE 0.810・iroha-bench-v1 0.783。
///
/// 判定するのは、表示しているライブ変換の結果がモデルの貪欲生成そのままのときだけ（`isSuspicious` の `shown`）。
/// 学習・ユーザ辞書・原稿の表記で差し替わった表示はユーザが選んだ結果なので疑わない。
/// AI への依頼の文面と答えの読み方は、測定で使ったもの（自由に直させる条件）と同じにしてある
public enum AIFixRule {

    /// これより長い読みは判定しない。ライブ変換は長い読みを区切って変換する
    /// （`ChunkedConversionEngine.defaultMaxChunkLength`）ので、1 回の生成の結果と表示が対応しない
    public static let maximumReadingLength = ChunkedConversionEngine.defaultMaxChunkLength

    /// 閾値の既定（nat）。iroha-bench-v1 で文の 27.9%、AJIMEE で 30.0% に窓が出る
    public static let defaultThreshold: Float = 1.0

    /// 文字ごとのマージンの最小値。競合するトークンが無かった文字（+inf）しかなければ +inf
    public static func minimumMargin(_ confidences: [CharacterConfidence]) -> Float {
        confidences.map(\.margin).min() ?? .infinity
    }

    /// 誤変換の疑いがあるか。
    /// - Parameters:
    ///   - scored: モデル単体で同じ読み・文脈を貪欲に変換した結果（マージンつき）
    ///   - shown: 表示しているライブ変換の結果
    ///   - reading: 読み
    ///   - threshold: マージンの最小値がこれより小さければ疑う
    public static func isSuspicious(
        _ scored: ScoredConversion, shown: String, reading: String, threshold: Float
    ) -> Bool {
        guard !reading.isEmpty, reading.count <= maximumReadingLength,
              scored.text == shown, !scored.confidences.isEmpty else { return false }
        return minimumMargin(scored.confidences) < threshold
    }

    /// AI への指示（測定の「自由に直させる」条件と同じ文面）
    public static let instructions = """
        あなたは日本語のかな漢字変換の結果を確かめる係です。入力として、前の文脈・読み（カタカナ）・変換結果が与えられます。
        変換結果に読みと合わない語や、文脈に合わない同音異義語があれば直してください。正しければそのまま返してください。
        読みにない語を足さないこと。言い換えないこと。句読点や記号を足さないこと。前の文脈は出力に含めないこと。
        出力は直した変換結果の1行だけにしてください。説明は書かないでください。
        """

    /// AI に渡す本文。読みはカタカナにして渡す（測定の評価セットと同じ形）
    public static func message(context: String, reading: String, conversion: String) -> String {
        let shownContext = context.isEmpty ? "（なし）" : context
        return "前の文脈: \(shownContext)\n読み: \(hiraganaToKatakana(reading))\n変換結果: \(conversion)"
    }

    /// 「今の変換 → AI の案」を出すための割り付け（打ち間違いの訂正の知らせと同じ形・同じ切り詰め方）。
    /// 前後の共通部分を除いた所を変わった部分とする。同じなら nil
    public static func feedback(
        original: String, corrected: String, context: Int = 6, maxChanged: Int = 12
    ) -> TypoCorrectionFeedback? {
        guard original != corrected else { return nil }
        let lhs = Array(original), rhs = Array(corrected)
        var head = 0
        while head < lhs.count, head < rhs.count, lhs[head] == rhs[head] { head += 1 }
        var tail = 0
        while tail < lhs.count - head, tail < rhs.count - head,
              lhs[lhs.count - 1 - tail] == rhs[rhs.count - 1 - tail] { tail += 1 }
        var prefix = String(lhs[..<head])
        if prefix.count > context { prefix = "…" + String(prefix.suffix(context)) }
        var suffix = String(lhs[(lhs.count - tail)...])
        if suffix.count > context { suffix = String(suffix.prefix(context)) + "…" }
        func abbreviate(_ characters: ArraySlice<Character>) -> String {
            characters.count > maxChanged ? String(characters.prefix(maxChanged)) + "…" : String(characters)
        }
        return TypoCorrectionFeedback(
            prefix: prefix,
            originalChanged: abbreviate(lhs[head..<(lhs.count - tail)]),
            correctedChanged: abbreviate(rhs[head..<(rhs.count - tail)]),
            suffix: suffix)
    }

    /// AI の答えから直した変換結果を取り出す。使えない答えなら nil。
    ///
    /// 説明が混ざって複数行になった答えは、最後の空でない行を答えとして使う（測定と同じ規則。
    /// Sonnet 5.5 は iroha-bench-v1 の 1,624 回のうち 218 回、説明のあとに答えを書いた）。
    /// 変換結果の 2 倍を超える長さは、文脈まで書き写したなどの崩れた答えとみなして捨てる
    public static func answer(from output: String, current: String) -> String? {
        let lines = output.split(whereSeparator: \.isNewline)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
        guard let last = lines.last else { return nil }
        guard last.count <= max(current.count * 2, current.count + 4) else { return nil }
        return last
    }
}
