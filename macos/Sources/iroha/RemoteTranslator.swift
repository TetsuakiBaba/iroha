import Foundation

/// ローカルLLMサーバ（Ollama / LM Studio）とOpenAI互換APIを使うAIバックエンド。
/// いずれもストリーミングで応答を受け、thinking（思考過程）は出力しないよう
/// リクエストで無効化し、混入した場合も<think>ブロックを除去する。
enum RemoteTranslator {

    enum Service {
        case ollama
        case lmstudio
        case openai  // OpenAI互換API（APIキー任意。LM Studioと同じSSE形式）

        var displayName: String {
            switch self {
            case .ollama: return "Ollama"
            case .lmstudio: return "LM Studio"
            case .openai: return "OpenAI互換"
            }
        }
    }

    // エンドポイントはUserDefaultsで上書き可能（Ollama/LM StudioはUIには出さない）
    static var ollamaEndpoint: String {
        UserDefaults.standard.string(forKey: "ollamaEndpoint") ?? "http://localhost:11434"
    }
    static var lmStudioEndpoint: String {
        UserDefaults.standard.string(forKey: "lmStudioEndpoint") ?? "http://localhost:1234"
    }
    static var ollamaModel: String {
        UserDefaults.standard.string(forKey: "ollamaModel") ?? ""
    }
    static var lmStudioModel: String {
        UserDefaults.standard.string(forKey: "lmStudioModel") ?? ""
    }
    static var openAIEndpoint: String {
        let raw = UserDefaults.standard.string(forKey: "openAIEndpoint") ?? ""
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? "https://api.openai.com/v1" : trimmed
    }
    static var openAIModel: String {
        UserDefaults.standard.string(forKey: "openAIModel") ?? ""
    }
    /// APIキーはKeychainに保存（平文plistを避ける）。ローカルプロキシ等では空でもよい
    static let openAIKeyAccount = "openAIAPIKey"
    static var openAIAPIKey: String {
        SecretStore.get(openAIKeyAccount) ?? ""
    }

    /// ベースURLの表記ゆれ（末尾スラッシュ・/v1の有無）を吸収して "…/v1" に揃える
    static func normalizedV1Base(_ endpoint: String) -> String {
        var base = endpoint.trimmingCharacters(in: .whitespacesAndNewlines)
        while base.hasSuffix("/") { base.removeLast() }
        if base.hasSuffix("/v1") { base.removeLast(3) }
        while base.hasSuffix("/") { base.removeLast() }
        return base + "/v1"
    }

    // MARK: - モデル一覧の取得（設定パネル用）

    static func listModels(service: Service) async throws -> [String] {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 3  // 未起動サーバを素早く検出
        let session = URLSession(configuration: config)
        switch service {
        case .ollama:
            struct Tags: Decodable {
                struct Model: Decodable { let name: String }
                let models: [Model]
            }
            let url = URL(string: ollamaEndpoint + "/api/tags")!
            let (data, _) = try await session.data(from: url)
            return try JSONDecoder().decode(Tags.self, from: data).models.map(\.name).sorted()
        case .lmstudio, .openai:
            struct ModelList: Decodable {
                struct Model: Decodable { let id: String }
                let data: [Model]
            }
            let endpoint = service == .lmstudio ? lmStudioEndpoint : openAIEndpoint
            let url = URL(string: normalizedV1Base(endpoint) + "/models")!
            var request = URLRequest(url: url)
            if service == .openai, !openAIAPIKey.isEmpty {
                request.setValue("Bearer \(openAIAPIKey)", forHTTPHeaderField: "Authorization")
            }
            let (data, _) = try await session.data(for: request)
            return try JSONDecoder().decode(ModelList.self, from: data).data.map(\.id).sorted()
        }
    }

    // MARK: - 実行（ストリーミング）

    static func run(
        _ request: AIRequest,
        service: Service,
        stallTimeout: TimeInterval,
        onPartial: @escaping @Sendable (String) -> Void
    ) async -> Result<String, AIFailure> {
        await TranslationService.runWithStallWatchdog(stallTimeout: stallTimeout) { progress in
            switch service {
            case .ollama:
                return try await streamOllama(request, progress: progress, onPartial: onPartial)
            case .lmstudio:
                return try await streamOpenAICompatible(
                    request, endpoint: lmStudioEndpoint, model: lmStudioModel, apiKey: "",
                    // LM Studioでthinkingを止める唯一効くフラグ（実測: qwen3で
                    // reasoning_tokensが58→0。chat_template_kwargsもlowも効かなかった）。
                    // 非thinkingモデルではLM Studio側が黙って無視する
                    extraBody: ["reasoning_effort": "none", "temperature": 0.3],
                    progress: progress, onPartial: onPartial)
            case .openai:
                // reasoning_effort・temperatureは送らない（OpenAI本家は reasoning_effort "none" を受け付けず、
                // 推論型のモデルは temperature 1 以外を受け付けない。どちらも400になる。2026-09-30 実測）
                return try await streamOpenAICompatible(
                    request, endpoint: openAIEndpoint, model: openAIModel, apiKey: openAIAPIKey,
                    extraBody: [:],
                    progress: progress, onPartial: onPartial)
            }
        }
    }

    private static func chatMessages(_ request: AIRequest) -> [[String: String]] {
        [
            ["role": "system", "content": request.instructions],
            ["role": "user", "content": request.userMessage],
        ]
    }

    /// Ollama /api/chat（JSONLストリーム）。"think": false でthinkingを無効化
    private static func streamOllama(
        _ request: AIRequest,
        progress: ProgressBox,
        onPartial: @escaping @Sendable (String) -> Void
    ) async throws -> String {
        var urlRequest = URLRequest(url: URL(string: ollamaEndpoint + "/api/chat")!)
        urlRequest.httpMethod = "POST"
        urlRequest.setValue("application/json", forHTTPHeaderField: "Content-Type")
        urlRequest.timeoutInterval = 300
        urlRequest.httpBody = try JSONSerialization.data(withJSONObject: [
            "model": ollamaModel,
            "messages": chatMessages(request),
            "stream": true,
            "think": false,  // qwen3等のthinkingモデルの思考出力を無効化
            "options": ["temperature": 0.3],
        ] as [String: Any])

        struct Chunk: Decodable {
            struct Message: Decodable { let content: String? }
            let message: Message?
            let done: Bool?
            let error: String?
        }

        let (bytes, response) = try await URLSession.shared.bytes(for: urlRequest)
        try await checkStatus(response, bytes: bytes)
        var accumulated = ""
        for try await line in bytes.lines {
            progress.bump()  // thinking中など本文が来ない間もサーバ活動があれば待ち続ける
            guard let data = line.data(using: .utf8),
                  let chunk = try? JSONDecoder().decode(Chunk.self, from: data) else { continue }
            if let error = chunk.error { throw RemoteError.serverError(error) }
            if let piece = chunk.message?.content, !piece.isEmpty {
                accumulated += piece
                onPartial(stripThinking(accumulated))
            }
            if chunk.done == true { break }
        }
        return stripThinking(accumulated)
    }

    /// /v1/chat/completions（OpenAI互換SSE）。LM StudioとOpenAI互換サービスの共通経路。
    /// reasoning系デルタはデコード対象外として無視し、<think>ブロックも除去する
    private static func streamOpenAICompatible(
        _ request: AIRequest,
        endpoint: String,
        model: String,
        apiKey: String,
        extraBody: [String: Any],
        progress: ProgressBox,
        onPartial: @escaping @Sendable (String) -> Void
    ) async throws -> String {
        var urlRequest = URLRequest(
            url: URL(string: normalizedV1Base(endpoint) + "/chat/completions")!)
        urlRequest.httpMethod = "POST"
        urlRequest.setValue("application/json", forHTTPHeaderField: "Content-Type")
        if !apiKey.isEmpty {
            urlRequest.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")
        }
        urlRequest.timeoutInterval = 300
        var body: [String: Any] = [
            "model": model,
            "messages": chatMessages(request),
            "stream": true,
        ]
        body.merge(extraBody) { _, new in new }
        urlRequest.httpBody = try JSONSerialization.data(withJSONObject: body)

        struct Chunk: Decodable {
            struct Choice: Decodable {
                struct Delta: Decodable { let content: String? }
                let delta: Delta?
            }
            let choices: [Choice]?
        }

        let (bytes, response) = try await URLSession.shared.bytes(for: urlRequest)
        try await checkStatus(response, bytes: bytes)
        var accumulated = ""
        for try await line in bytes.lines {
            progress.bump()  // reasoning中も接続が生きていれば待ち続ける
            guard line.hasPrefix("data: ") else { continue }
            let payload = String(line.dropFirst(6))
            if payload == "[DONE]" { break }
            guard let data = payload.data(using: .utf8),
                  let chunk = try? JSONDecoder().decode(Chunk.self, from: data) else { continue }
            if let piece = chunk.choices?.first?.delta?.content, !piece.isEmpty {
                accumulated += piece
                onPartial(stripThinking(accumulated))
            }
        }
        return stripThinking(accumulated)
    }

    /// 200以外なら、応答の本文からサーバのエラー文を取り出して投げる
    private static func checkStatus(_ response: URLResponse, bytes: URLSession.AsyncBytes) async throws {
        let code = (response as? HTTPURLResponse)?.statusCode ?? 0
        guard code != 200 else { return }
        var body = ""
        do {
            for try await line in bytes.lines {
                body += line + "\n"
                if body.count > 4000 { break }
            }
        } catch {}
        throw RemoteError.httpError(code, errorMessage(fromBody: body))
    }

    /// エラー応答の本文からメッセージを取り出す（OpenAI: {"error":{"message":…}}、
    /// Ollama・LM Studio: {"error":"…"}）。JSONでなければ本文の先頭を返す
    static func errorMessage(fromBody body: String) -> String? {
        let trimmed = body.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        if let data = trimmed.data(using: .utf8),
           let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            if let error = json["error"] as? [String: Any], let message = error["message"] as? String {
                return message
            }
            if let message = json["error"] as? String { return message }
        }
        return String(trimmed.prefix(300))
    }

    enum RemoteError: LocalizedError {
        case httpError(Int, String?)
        case serverError(String)

        var errorDescription: String? {
            switch self {
            case .httpError(let code, let message):
                return message.map { "HTTP \(code): \($0)" } ?? "HTTP \(code)"
            case .serverError(let message): return message
            }
        }
    }

    /// <think>...</think>ブロックを除去する（think無効化をすり抜けた場合の保険）。
    /// 閉じタグ未到達の間は思考中とみなし、そこまでの本文だけを返す
    static func stripThinking(_ text: String) -> String {
        var result = text
        while let start = result.range(of: "<think>") {
            if let end = result.range(of: "</think>", range: start.upperBound..<result.endIndex) {
                result.removeSubrange(start.lowerBound..<end.upperBound)
            } else {
                result = String(result[..<start.lowerBound])
                break
            }
        }
        return result.trimmingCharacters(in: .whitespacesAndNewlines)
    }
}
