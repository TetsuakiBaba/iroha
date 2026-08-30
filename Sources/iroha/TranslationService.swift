import Foundation
#if canImport(FoundationModels)
import FoundationModels
#endif

/// 日本語→英語のオンデバイス翻訳（Apple FoundationModels / macOS 26+）。
/// 利用不可・失敗時はnilを返し、呼び出し側が日本語をそのまま確定する。
enum TranslationService {

    static var isAvailable: Bool {
        #if canImport(FoundationModels)
        if #available(macOS 26.0, *) {
            if case .available = SystemLanguageModel.default.availability { return true }
        }
        #endif
        return false
    }

    #if canImport(FoundationModels)
    /// prewarm用に生かしておくセッション（一時オブジェクトだとdeinitでプリウォームが
    /// キャンセルされ、フォーカス切替のたびに無駄なセッション作成/削除が走る）
    @available(macOS 26.0, *)
    private enum Prewarm {
        static let session = LanguageModelSession(instructions: TranslationService.instructions)
        nonisolated(unsafe) static var done = false
    }
    #endif

    /// モデルの事前ロードを促す（プロセスで一度だけ。失敗しても無害）
    static func prewarm() {
        #if canImport(FoundationModels)
        if #available(macOS 26.0, *), isAvailable, !Prewarm.done {
            Prewarm.done = true
            Prewarm.session.prewarm()
        }
        #endif
    }

    private static let instructions = """
        You are a Japanese-to-English translation engine. \
        Translate the user's Japanese text into natural English. \
        Output ONLY the English translation - no quotation marks, no romaji, \
        no notes, no explanations, no alternatives.
        """

    /// ストリーミング翻訳。途中経過（累積の英文）をonPartialへ随時渡し、完成文を返す。
    /// stallTimeoutの間トークンが1つも進まなければ打ち切ってnilを返す
    /// （進捗がある限りは打ち切らない。呼び出し側はnilで日本語にフォールバックする）
    static func translate(
        _ japanese: String,
        stallTimeout: TimeInterval = 10,
        onPartial: @escaping @Sendable (String) -> Void = { _ in }
    ) async -> String? {
        #if canImport(FoundationModels)
        guard #available(macOS 26.0, *), isAvailable else { return nil }
        // セッションは毎回作る: 履歴が翻訳結果に影響しないよう常にステートレスにする
        let session = LanguageModelSession(instructions: instructions)
        let progress = ProgressBox()
        let respondTask = Task { () -> String in
            var latest = ""
            let stream = session.streamResponse(
                to: japanese,
                options: GenerationOptions(temperature: 0.3)
            )
            for try await partial in stream {
                latest = partial.content
                progress.bump()
                onPartial(latest)
            }
            return latest
        }
        // ストール監視: 一定時間進捗がなければキャンセル（生成が続く限りは待つ）
        let watchdog = Task {
            var last = progress.value
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: UInt64(stallTimeout * 1_000_000_000))
                if Task.isCancelled { return }
                let now = progress.value
                if now == last {
                    respondTask.cancel()
                    return
                }
                last = now
            }
        }
        defer { watchdog.cancel() }
        do {
            let text = try await respondTask.value
                .trimmingCharacters(in: .whitespacesAndNewlines)
            return text.isEmpty ? nil : text
        } catch {
            // ガードレール拒否・レート制限・ストール打ち切り（キャンセル）もここに落ちる
            NSLog("iroha: 翻訳エラー: \(error)")
            return nil
        }
        #else
        return nil
        #endif
    }
}

/// ストリーム進捗のスレッド安全なカウンタ（ストール監視用）
private final class ProgressBox: @unchecked Sendable {
    private let lock = NSLock()
    private var count = 0
    func bump() {
        lock.lock()
        count += 1
        lock.unlock()
    }
    var value: Int {
        lock.lock()
        defer { lock.unlock() }
        return count
    }
}
