import Foundation

/// ローカルLLMサーバ（Ollama / LM Studio）を使った翻訳バックエンド。
/// どちらもストリーミングで応答を受け、thinking（思考過程）は出力しないよう
/// リクエストで無効化し、混入した場合も<think>ブロックを除去する。
enum RemoteTranslator {

    enum Service {
        case ollama
        case lmstudio

        var displayName: String {
            switch self {
            case .ollama: return "Ollama"
            case .lmstudio: return "LM Studio"
            }
        }
    }

    // エンドポイントはUserDefaultsで上書き可能（UIには出さない）
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
        case .lmstudio:
            struct ModelList: Decodable {
                struct Model: Decodable { let id: String }
                let data: [Model]
            }
            let url = URL(string: lmStudioEndpoint + "/v1/models")!
            let (data, _) = try await session.data(from: url)
            return try JSONDecoder().decode(ModelList.self, from: data).data.map(\.id).sorted()
        }
    }

    // MARK: - 翻訳（ストリーミング）

    static func translate(
        _ japanese: String,
        service: Service,
        stallTimeout: TimeInterval,
        onPartial: @escaping @Sendable (String) -> Void
    ) async -> String? {
        await TranslationService.runWithStallWatchdog(stallTimeout: stallTimeout) { progress in
            switch service {
            case .ollama:
                return try await streamOllama(japanese, progress: progress, onPartial: onPartial)
            case .lmstudio:
                return try await streamLMStudio(japanese, progress: progress, onPartial: onPartial)
            }
        }
    }

    private static func chatMessages(_ japanese: String) -> [[String: String]] {
        [
            ["role": "system", "content": TranslationService.instructions],
            ["role": "user", "content": japanese],
        ]
    }

    /// Ollama /api/chat（JSONLストリーム）。"think": false でthinkingを無効化
    private static func streamOllama(
        _ japanese: String,
        progress: ProgressBox,
        onPartial: @escaping @Sendable (String) -> Void
    ) async throws -> String {
        var request = URLRequest(url: URL(string: ollamaEndpoint + "/api/chat")!)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.timeoutInterval = 300
        request.httpBody = try JSONSerialization.data(withJSONObject: [
            "model": ollamaModel,
            "messages": chatMessages(japanese),
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

        let (bytes, response) = try await URLSession.shared.bytes(for: request)
        guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
            throw RemoteError.httpError((response as? HTTPURLResponse)?.statusCode ?? 0)
        }
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

    /// LM Studio /v1/chat/completions（OpenAI互換SSE）。
    /// reasoning系デルタはデコード対象外として無視し、<think>ブロックも除去する
    private static func streamLMStudio(
        _ japanese: String,
        progress: ProgressBox,
        onPartial: @escaping @Sendable (String) -> Void
    ) async throws -> String {
        var request = URLRequest(url: URL(string: lmStudioEndpoint + "/v1/chat/completions")!)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.timeoutInterval = 300
        request.httpBody = try JSONSerialization.data(withJSONObject: [
            "model": lmStudioModel,
            "messages": chatMessages(japanese),
            "stream": true,
            "temperature": 0.3,
        ] as [String: Any])

        struct Chunk: Decodable {
            struct Choice: Decodable {
                struct Delta: Decodable { let content: String? }
                let delta: Delta?
            }
            let choices: [Choice]?
        }

        let (bytes, response) = try await URLSession.shared.bytes(for: request)
        guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
            throw RemoteError.httpError((response as? HTTPURLResponse)?.statusCode ?? 0)
        }
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

    enum RemoteError: LocalizedError {
        case httpError(Int)
        case serverError(String)

        var errorDescription: String? {
            switch self {
            case .httpError(let code): return "HTTP \(code)"
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
