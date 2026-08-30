import Foundation
#if canImport(FoundationModels)
import FoundationModels
#endif

/// 翻訳バックエンドの種類（設定パネルで選択）
enum TranslationBackend: String, CaseIterable {
    case apple      // Apple FoundationModels（オンデバイス、macOS 26+）
    case ollama     // ローカルのOllamaサーバ
    case lmstudio   // ローカルのLM Studioサーバ

    static let userDefaultsKey = "translationService"

    static var current: TranslationBackend {
        TranslationBackend(
            rawValue: UserDefaults.standard.string(forKey: userDefaultsKey) ?? "apple"
        ) ?? .apple
    }
}

/// 日本語→英語翻訳のディスパッチャ。
/// バックエンド（Apple / Ollama / LM Studio）を設定に応じて切り替える。
/// 利用不可・失敗時はnilを返し、呼び出し側が日本語をそのまま確定する。
enum TranslationService {

    static var isAvailable: Bool {
        switch TranslationBackend.current {
        case .apple:
            return appleAvailable
        case .ollama:
            return !RemoteTranslator.ollamaModel.isEmpty
        case .lmstudio:
            return !RemoteTranslator.lmStudioModel.isEmpty
        }
    }

    static var appleAvailable: Bool {
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

    /// モデルの事前ロードを促す（Appleバックエンド時のみ・プロセスで一度だけ。失敗しても無害）
    static func prewarm() {
        #if canImport(FoundationModels)
        if #available(macOS 26.0, *), TranslationBackend.current == .apple,
           appleAvailable, !Prewarm.done {
            Prewarm.done = true
            Prewarm.session.prewarm()
        }
        #endif
    }

    static let instructions = """
        You are a Japanese-to-English translation engine. \
        Translate the user's Japanese text into natural English. \
        Output ONLY the English translation - no quotation marks, no romaji, \
        no notes, no explanations, no alternatives.
        """

    /// ストリーミング翻訳。途中経過（累積の英文）をonPartialへ随時渡し、完成文を返す。
    /// ストールタイムアウトの間サーバから何も届かなければ打ち切ってnilを返す
    /// （進捗がある限りは打ち切らない。呼び出し側はnilで日本語にフォールバックする）
    static func translate(
        _ japanese: String,
        onPartial: @escaping @Sendable (String) -> Void = { _ in }
    ) async -> String? {
        switch TranslationBackend.current {
        case .apple:
            return await translateWithApple(japanese, stallTimeout: 10, onPartial: onPartial)
        case .ollama:
            // ローカルLLMはモデルのコールドロードに時間がかかることがあるため長めに待つ
            return await RemoteTranslator.translate(
                japanese, service: .ollama, stallTimeout: 30, onPartial: onPartial)
        case .lmstudio:
            return await RemoteTranslator.translate(
                japanese, service: .lmstudio, stallTimeout: 30, onPartial: onPartial)
        }
    }

    private static func translateWithApple(
        _ japanese: String,
        stallTimeout: TimeInterval,
        onPartial: @escaping @Sendable (String) -> Void
    ) async -> String? {
        #if canImport(FoundationModels)
        guard #available(macOS 26.0, *), appleAvailable else { return nil }
        // セッションは毎回作る: 履歴が翻訳結果に影響しないよう常にステートレスにする
        let session = LanguageModelSession(instructions: instructions)
        return await runWithStallWatchdog(stallTimeout: stallTimeout) { progress in
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
        #else
        return nil
        #endif
    }

    /// ストール監視付きで生成処理を実行する共通ヘルパー。
    /// stallTimeoutの間progressが進まなければ処理をキャンセルしnilを返す。
    /// エラー（ガードレール拒否・接続失敗・キャンセル）もnilに落とす
    static func runWithStallWatchdog(
        stallTimeout: TimeInterval,
        _ body: @escaping @Sendable (ProgressBox) async throws -> String
    ) async -> String? {
        let progress = ProgressBox()
        let task = Task { try await body(progress) }
        let watchdog = Task {
            var last = progress.value
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: UInt64(stallTimeout * 1_000_000_000))
                if Task.isCancelled { return }
                let now = progress.value
                if now == last {
                    task.cancel()
                    return
                }
                last = now
            }
        }
        defer { watchdog.cancel() }
        do {
            let text = try await task.value
                .trimmingCharacters(in: .whitespacesAndNewlines)
            return text.isEmpty ? nil : text
        } catch {
            NSLog("iroha: 翻訳エラー: \(error)")
            return nil
        }
    }
}

/// ストリーム進捗のスレッド安全なカウンタ（ストール監視用）
final class ProgressBox: @unchecked Sendable {
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
