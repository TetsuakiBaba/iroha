import Cocoa
import IrohaCore

/// 英数モードへの切り替えを忘れて英語を打ったとき、打鍵どおりの英字を候補ウィンドウに足す設定
/// （開発者モードの項目、既定OFF・他のMacと同期しない。2026-10-03）。
///
/// 自動では切り替えず、スペースで文節変換に入ったときの候補に足すだけ。何を候補にするかは
/// `EnglishInputDetector` が決め、英単語かどうかは macOS のスペルチェッカー（英語）で見る
enum EnglishCandidateSettings {

    static let enabledKey = "englishCandidates"

    static var isEnabled: Bool {
        UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? false
    }

    /// 合成中の打鍵から英字の候補を作る。設定OFF・打鍵と読みの対応が崩れているときは nil。
    /// `NSSpellChecker` はメインスレッドで使うので、キー処理の中から呼ぶ
    static func candidate(for composer: RomajiComposer) -> EnglishInputDetector.Candidate? {
        guard isEnabled, composer.rawIsReliable else { return nil }
        return EnglishInputDetector.candidate(
            raw: composer.raw, reading: composer.text, isEnglishWord: isEnglishWord)
    }

    private static func isEnglishWord(_ word: String) -> Bool {
        let range = NSSpellChecker.shared.checkSpelling(
            of: word, startingAt: 0, language: "en", wrap: false,
            inSpellDocumentWithTag: 0, wordCount: nil)
        return range.location == NSNotFound
    }
}
