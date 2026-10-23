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
    case claude     // Claude（Anthropic API。APIキーはKeychain。テキストが外部に送られる）

    static let userDefaultsKey = "translationService"

    static var current: TranslationBackend {
        TranslationBackend(
            rawValue: UserDefaults.standard.string(forKey: userDefaultsKey) ?? "apple"
        ) ?? .apple
    }

    var displayName: String {
        switch self {
        case .apple: return "Apple Intelligence"
        case .ollama: return "Ollama"
        case .lmstudio: return "LM Studio"
        case .openai: return "OpenAI互換"
        case .claude: return "Claude"
        }
    }
}

/// AI処理が失敗した理由（選択テキストの結果画面に出す。AI変換は理由を出さず日本語のまま確定する）
struct AIFailure: Error {
    let message: String
}

/// AI変換（英訳・校正など）と選択テキストのAI編集のディスパッチャ。
/// バックエンド（Apple / Ollama / LM Studio / OpenAI互換 / Claude）を設定に応じて切り替える。
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
        case .claude:
            return !RemoteTranslator.claudeAPIKey.isEmpty
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
        static let lock = NSLock()
        nonisolated(unsafe) static var last: Date?
    }
    #endif

    /// 続けて prewarm を頼まない間隔
    static let prewarmInterval: TimeInterval = 30

    /// モデルの事前ロードを促す（Appleバックエンド時のみ。失敗しても無害）。
    ///
    /// フォーカスが来たときと入力の始まりに呼ぶ。システムは使われていないモデルをしばらくすると外し、
    /// 外れた状態で頼むと 1 回に 6.5〜7.5 秒かかる（読み込み済みなら 0.5〜1 秒。2026-10-23 実測）。
    /// プロセスで一度だけでは、時間がたって外されたあとに効かないので、間隔をあけて頼み直す
    static func prewarm() {
        #if canImport(FoundationModels)
        guard #available(macOS 26.0, *), TranslationBackend.current == .apple, appleAvailable
        else { return }
        Prewarm.lock.lock()
        let now = Date()
        let due = Prewarm.last.map { now.timeIntervalSince($0) >= prewarmInterval } ?? true
        if due { Prewarm.last = now }
        Prewarm.lock.unlock()
        if due { Prewarm.session.prewarm() }
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
        try? await runReportingFailure(request, onPartial: onPartial).get()
    }

    /// `run` と同じ処理を、失敗したときの理由（HTTPの番号とサーバのエラー文・接続できない理由・
    /// 応答が止まった秒数）つきで返す
    static func runReportingFailure(
        _ request: AIRequest,
        onPartial: @escaping @Sendable (String) -> Void = { _ in }
    ) async -> Result<String, AIFailure> {
        switch TranslationBackend.current {
        case .apple:
            // 途中経過はほぼ最後にまとめて届くので、ここでの打ち切りは全体の時間の上限として効く。
            // モデルが外れていると 1 回に 6.5〜7.5 秒、10 秒を超えたこともある（2026-10-23 実測）
            return await runWithApple(request, stallTimeout: 30, onPartial: onPartial)
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
        case .claude:
            return await RemoteTranslator.run(
                request, service: .claude, stallTimeout: 30, onPartial: onPartial)
        }
    }

    private static func runWithApple(
        _ request: AIRequest,
        stallTimeout: TimeInterval,
        onPartial: @escaping @Sendable (String) -> Void
    ) async -> Result<String, AIFailure> {
        #if canImport(FoundationModels)
        guard #available(macOS 26.0, *), appleAvailable else {
            return .failure(AIFailure(message: "Apple Intelligence が使えません"))
        }
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
        return .failure(AIFailure(message: "Apple Intelligence が使えません"))
        #endif
    }

    /// ストール監視付きで生成処理を実行する共通ヘルパー。
    /// stallTimeoutの間progressが進まなければ処理をキャンセルして失敗にする。
    /// エラー（ガードレール拒否・接続失敗・HTTPエラー）も理由つきの失敗にする
    static func runWithStallWatchdog(
        stallTimeout: TimeInterval,
        _ body: @escaping @Sendable (ProgressBox) async throws -> String
    ) async -> Result<String, AIFailure> {
        let progress = ProgressBox()
        let task = Task { try await body(progress) }
        let watchdog = Task {
            var last = progress.value
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: UInt64(stallTimeout * 1_000_000_000))
                if Task.isCancelled { return }
                let now = progress.value
                if now == last {
                    progress.markStalled()
                    task.cancel()
                    return
                }
                last = now
            }
        }
        defer { watchdog.cancel() }
        let stalledFailure = AIFailure(message: "\(Int(stallTimeout))秒間応答がありませんでした")
        do {
            let text = try await task.value
                .trimmingCharacters(in: .whitespacesAndNewlines)
            // Apple のストリームは打ち切られても例外を出さず、そこまでの出力で終わる
            if progress.stalled { return .failure(stalledFailure) }
            return text.isEmpty ? .failure(AIFailure(message: "空の応答が返りました")) : .success(text)
        } catch {
            NSLog("iroha: AI変換エラー: \(error)")
            if progress.stalled { return .failure(stalledFailure) }
            return .failure(AIFailure(message: error.localizedDescription))
        }
    }
}

/// ストリーム進捗のスレッド安全なカウンタ（ストール監視用）
final class ProgressBox: @unchecked Sendable {
    private let lock = NSLock()
    private var count = 0
    private var didStall = false
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
    /// ストール監視が打ち切った（失敗の理由を「応答なし」と区別するため）
    func markStalled() {
        lock.lock()
        didStall = true
        lock.unlock()
    }
    var stalled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return didStall
    }
}
