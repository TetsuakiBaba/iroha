import Foundation
#if canImport(FoundationModels)
import FoundationModels
#endif

/// 翻訳バックエンドの種類（設定パネルで選択）
enum TranslationBackend: String, CaseIterable {
    case apple      // Apple FoundationModels（オンデバイス、macOS 26+）
    case ollama     // ローカルのOllamaサーバ
    case lmstudio   // ローカルのLM Studioサーバ
    case openai     // OpenAI互換API（APIキーはKeychain。テキストが外部に送られる）

    static let userDefaultsKey = "translationService"

    static var current: TranslationBackend {
        TranslationBackend(
            rawValue: UserDefaults.standard.string(forKey: userDefaultsKey) ?? "apple"
        ) ?? .apple
    }
}

/// AI確定（英訳・AI変換）のディスパッチャ。
/// バックエンド（Apple / Ollama / LM Studio）を設定に応じて切り替える。
/// 利用不可・失敗時はnilを返し、呼び出し側が元の日本語をそのまま確定する。
enum TranslationService {

    static var isAvailable: Bool {
        switch TranslationBackend.current {
        case .apple:
            return appleAvailable
        case .ollama:
            return !RemoteTranslator.ollamaModel.isEmpty
        case .lmstudio:
            return !RemoteTranslator.lmStudioModel.isEmpty
        case .openai:
            return !RemoteTranslator.openAIModel.isEmpty
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
        static let session = LanguageModelSession(
            instructions: TranslationService.translateInstructions)
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

    static let translateInstructions = """
        You are a Japanese-to-English translation engine. \
        Translate the user's Japanese text into natural English. \
        Output ONLY the English translation - no quotation marks, no romaji, \
        no notes, no explanations, no alternatives.
        """

    /// ストリーミング実行。途中経過（累積の出力）をonPartialへ随時渡し、完成文を返す。
    /// ストールタイムアウトの間サーバから何も届かなければ打ち切ってnilを返す
    /// （進捗がある限りは打ち切らない。呼び出し側はnilで元の日本語にフォールバックする）
    static func run(
        _ request: AIRequest,
        onPartial: @escaping @Sendable (String) -> Void = { _ in }
    ) async -> String? {
        switch TranslationBackend.current {
        case .apple:
            return await runWithApple(request, stallTimeout: 10, onPartial: onPartial)
        case .ollama:
            // ローカルLLMはモデルのコールドロードに時間がかかることがあるため長めに待つ
            return await RemoteTranslator.run(
                request, service: .ollama, stallTimeout: 30, onPartial: onPartial)
        case .lmstudio:
            return await RemoteTranslator.run(
                request, service: .lmstudio, stallTimeout: 30, onPartial: onPartial)
        case .openai:
            return await RemoteTranslator.run(
                request, service: .openai, stallTimeout: 30, onPartial: onPartial)
        }
    }

    private static func runWithApple(
        _ request: AIRequest,
        stallTimeout: TimeInterval,
        onPartial: @escaping @Sendable (String) -> Void
    ) async -> String? {
        #if canImport(FoundationModels)
        guard #available(macOS 26.0, *), appleAvailable else { return nil }
        // セッションは毎回作る: 履歴が結果に影響しないよう常にステートレスにする
        let session = LanguageModelSession(instructions: request.instructions)
        return await runWithStallWatchdog(stallTimeout: stallTimeout) { progress in
            var latest = ""
            let stream = session.streamResponse(
                to: request.userMessage,
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
            NSLog("iroha: AI確定エラー: \(error)")
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
