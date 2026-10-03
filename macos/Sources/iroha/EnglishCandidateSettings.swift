import Cocoa
import IrohaCore

/// 英数モードへの切り替えを忘れて英語を打ったとき、打鍵どおりの英字を候補ウィンドウに足す設定
/// （開発者モードの項目、既定OFF・他のMacと同期しない。2026-10-03）。
///
/// 自動では切り替えず、スペースで文節変換に入ったときの候補に足すだけ。何を候補にするかは
/// `EnglishInputDetector` が決め、英単語かどうかは macOS のスペルチェッカー（英語）で見る。
///
/// もう一つの設定 `autoRewriteKey`（同じく開発者モード・既定OFF・同期しない。2026-10-04）は、
/// 入力の休止で、打鍵の中の英単語を読みのうえで英字に書き換える（打ち間違いの訂正より先に）。
/// 対象はローマ字で読めない英字を含む英単語だけ（`EnglishInputDetector.rewrite`）
enum EnglishCandidateSettings {

    static let enabledKey = "englishCandidates"
    static let autoRewriteKey = "englishAutoRewrite"

    static var isEnabled: Bool {
        UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? false
    }

    static var isAutoRewriteEnabled: Bool {
        UserDefaults.standard.object(forKey: autoRewriteKey) as? Bool ?? false
    }

    /// 入力の休止で、打鍵の中の英単語を英字にした読み。設定OFF・打鍵が読み全体に対応しない・
    /// 英字にする語がないときは nil。`excluding` は Backspace で取り消された語
    static func rewrite(_ composer: RomajiComposer, excluding: Set<String>) -> EnglishInputDetector.Rewrite? {
        guard isAutoRewriteEnabled, composer.rawCoversInput else { return nil }
        return EnglishInputDetector.rewrite(raw: composer.raw, excluding: excluding, isEnglishWord: isEnglishWord)
    }

    /// 判定の結果（候補にしなかったときは理由。デバッグ表示に出す）
    enum Outcome {
        case candidate(EnglishInputDetector.Candidate)
        /// 途中でかなを消した・固定した部分を読みに戻したので、打鍵が読み全体に対応しない
        case rawLost
        case rejected(EnglishInputDetector.Rejection)

        var candidate: EnglishInputDetector.Candidate? {
            if case .candidate(let candidate) = self { return candidate }
            return nil
        }

        var description: String {
            switch self {
            case .candidate(let candidate):
                let place = candidate.placement == .near ? "第一候補の近く" : "採点した上位の後ろ"
                return "「\(candidate.word)」を候補に出す（\(place)）"
            case .rawLost: return "出さない（かなを消した・読みを戻したので打鍵が残っていない）"
            case .rejected(.notWordShaped): return "出さない（英字以外を含む・2文字未満）"
            case .rejected(.romaji): return "出さない（ローマ字として読め、英単語でもない）"
            }
        }
    }

    /// 合成中の打鍵から英字の候補を判定する。設定OFFなら nil。
    /// `NSSpellChecker` はメインスレッドで使うので、キー処理の中から呼ぶ
    static func evaluate(_ composer: RomajiComposer) -> Outcome? {
        guard isEnabled else { return nil }
        guard composer.rawCoversInput else { return .rawLost }
        switch EnglishInputDetector.detect(raw: composer.raw, isEnglishWord: isEnglishWord) {
        case .candidate(let candidate): return .candidate(candidate)
        case .rejected(let rejection): return .rejected(rejection)
        }
    }

    private static func isEnglishWord(_ word: String) -> Bool {
        let range = NSSpellChecker.shared.checkSpelling(
            of: word, startingAt: 0, language: "en", wrap: false,
            inSpellDocumentWithTag: 0, wordCount: nil)
        return range.location == NSNotFound
    }
}
