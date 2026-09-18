import Foundation

/// 記録を「いまのモデルが間違えるもの」と「正解できるもの」に振り分ける。
///
/// 追加学習で変えられるのはニューラルネット（`ZenzEngine`）の出力だけなので、学習の対象は
/// **NN が実際に間違えた記録**でなければならない。記録の `proposed` / `edited` は辞書ラティスや
/// 学習・ユーザ辞書を含む「エンジン全体」の提示に対する差分なので、NN の誤りとは一致しない
/// （実測: ユーザが直した 11 件のうち NN は 6 件を既に正解し、逆に NN が間違える 12 件のうち
/// 7 件はユーザが直していなかった＝辞書や学習に助けられていた）。
/// ここで選り分けると、学習前の一致数が「間違えた群 0 件・正解群は全件」と自明になるので、
/// 学習後の数値がそのまま効果と副作用になる
public enum TrainingScreener {

    public struct Screening: Sendable {
        /// いまのモデルが間違える記録（学習の対象）
        public var mistakes: [ConversionLogEntry] = []
        /// いまのモデルが正解できる記録（アンカー・壊れていないかの確認用）
        public var correct: [ConversionLogEntry] = []
    }

    /// 一度に確認する記録の上限（新しいものを優先。1 件 20ms 程度かかるため）
    public static let defaultLimit = 1500

    public static func screen(entries: [ConversionLogEntry], modelPath: String, limit: Int = defaultLimit,
                              progress: (Int, Int) -> Void = { _, _ in }) async throws -> Screening
    {
        let targets = Array(entries.suffix(limit))
        let engine = ZenzEngine(modelPath: modelPath)
        try await (engine as any ConversionEngine).prewarm()
        var screening = Screening()
        for (index, entry) in targets.enumerated() {
            let output = try await engine.convert(reading: entry.reading, context: entry.context, candidateCount: 1).first ?? ""
            if output == entry.committed {
                screening.correct.append(entry)
            } else {
                screening.mistakes.append(entry)
            }
            progress(index + 1, targets.count)
        }
        return screening
    }
}
