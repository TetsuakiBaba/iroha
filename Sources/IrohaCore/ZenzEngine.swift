import CLlama
import Foundation

/// zenz-v3（GPT-2系かな漢字変換モデル）をllama.cppで動かす変換エンジン。
///
/// プロンプト形式（zenz-v3）:
///   [U+EE02 + 左文脈] + U+EE00 + カタカナ読み + U+EE01 → 変換結果
public actor ZenzEngine: ConversionEngine {

    public static let defaultModelPath = NSHomeDirectory()
        + "/Library/Application Support/iroha/models/zenz-v3.1-small-Q5_K_M.gguf"

    /// llama.cppのリソース一式。deinitで確実に解放する
    private final class Runtime: @unchecked Sendable {
        let model: OpaquePointer
        let context: OpaquePointer
        let vocab: OpaquePointer
        let sampler: UnsafeMutablePointer<llama_sampler>

        init(model: OpaquePointer, context: OpaquePointer, vocab: OpaquePointer,
             sampler: UnsafeMutablePointer<llama_sampler>) {
            self.model = model
            self.context = context
            self.vocab = vocab
            self.sampler = sampler
        }

        deinit {
            llama_sampler_free(sampler)
            llama_free(context)
            llama_model_free(model)
        }
    }

    private let modelPath: String
    private var runtime: Runtime?

    /// 左文脈として与える最大文字数（zenz-v3の学習設定に合わせる）
    private let maxContextLength = 40

    public init(modelPath: String = ZenzEngine.defaultModelPath) {
        self.modelPath = modelPath
    }

    /// モデルを事前にロードしておく（初回変換のもたつき防止）
    public func prewarm() throws {
        try ensureLoaded()
    }

    // MARK: - ConversionEngine

    public func convert(reading: String, context leftContext: String, candidateCount: Int) async throws -> [String] {
        try ensureLoaded()
        guard let runtime else {
            throw ConversionError.modelLoadFailed("内部状態が不正です")
        }

        let prompt = Self.buildPrompt(reading: reading, leftContext: leftContext, maxContextLength: maxContextLength)
        let promptTokens = try tokenize(prompt, addSpecial: true)

        if candidateCount <= 1 {
            let result = try generate(runtime: runtime, promptTokens: promptTokens,
                                      forcedFirstToken: nil, readingLength: reading.count)
            return [result.isEmpty ? reading : result]
        }

        // n-best: 先頭トークンを上位候補に分岐し、それぞれ貪欲に補完する。
        // かな漢字変換では先頭の文字が同音異義語をほぼ決めるため、これで多様な候補が得られる
        var tokens = promptTokens
        llama_memory_clear(llama_get_memory(runtime.context), true)
        try decode(ctx: runtime.context, tokens: &tokens)

        let firstTokens = topTokens(runtime: runtime, count: candidateCount * 3)
        var results: [String] = []
        for token in firstTokens {
            try Task.checkCancellation()
            if llama_vocab_is_eog(runtime.vocab, token) { continue }
            let text = try generate(runtime: runtime, promptTokens: promptTokens,
                                    forcedFirstToken: token, readingLength: reading.count)
            if !text.isEmpty, !results.contains(text) {
                results.append(text)
            }
            if results.count >= candidateCount { break }
        }
        if results.isEmpty { results = [reading] }
        return results
    }

    /// プロンプト評価直後のlogitsから上位トークンを返す
    private func topTokens(runtime: Runtime, count: Int) -> [llama_token] {
        guard let logits = llama_get_logits_ith(runtime.context, -1) else { return [] }
        let vocabSize = Int(llama_vocab_n_tokens(runtime.vocab))
        var indexed: [(token: llama_token, logit: Float)] = []
        indexed.reserveCapacity(vocabSize)
        for index in 0..<vocabSize {
            indexed.append((llama_token(index), logits[index]))
        }
        return indexed.sorted { $0.logit > $1.logit }.prefix(count).map(\.token)
    }

    /// 貪欲法で1候補を生成する。forcedFirstTokenがあれば先頭をそのトークンに固定する
    private func generate(
        runtime: Runtime,
        promptTokens: [llama_token],
        forcedFirstToken: llama_token?,
        readingLength: Int
    ) throws -> String {
        let ctx = runtime.context
        let vocab = runtime.vocab
        var tokens = promptTokens
        if let forcedFirstToken {
            tokens.append(forcedFirstToken)
        }

        // KVキャッシュを破棄してプロンプトを評価（TODO: プレフィックス再利用で増分デコード）
        llama_memory_clear(llama_get_memory(ctx), true)
        try decode(ctx: ctx, tokens: &tokens)

        var outputBytes = Data()
        var pieceBuffer = [CChar](repeating: 0, count: 128)

        func appendPiece(_ token: llama_token) {
            let written = llama_token_to_piece(vocab, token, &pieceBuffer, Int32(pieceBuffer.count), 0, true)
            if written > 0 {
                pieceBuffer.withUnsafeBytes { raw in
                    outputBytes.append(raw.baseAddress!.assumingMemoryBound(to: UInt8.self), count: Int(written))
                }
            }
        }
        if let forcedFirstToken { appendPiece(forcedFirstToken) }

        let maxNewTokens = readingLength * 3 + 8
        generation: for _ in 0..<maxNewTokens {
            try Task.checkCancellation()
            let token = llama_sampler_sample(runtime.sampler, ctx, -1)
            if llama_vocab_is_eog(vocab, token) { break }

            appendPiece(token)
            // zenzの特殊トークン（私用領域 U+EE00-U+EE0F）が出たら終了
            if let text = String(data: outputBytes, encoding: .utf8),
               let last = text.unicodeScalars.last, (0xEE00...0xEE0F).contains(last.value) {
                outputBytes = Data(String(text.unicodeScalars.dropLast()).utf8)
                break generation
            }

            var next = token
            try decode(ctx: ctx, tokens: &next)
        }

        guard let output = String(data: outputBytes, encoding: .utf8) else {
            throw ConversionError.inferenceFailed("出力がUTF-8として不正です")
        }
        return output.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    static func buildPrompt(reading: String, leftContext: String, maxContextLength: Int) -> String {
        var prompt = ""
        if !leftContext.isEmpty {
            prompt += "\u{EE02}" + String(leftContext.suffix(maxContextLength))
        }
        prompt += "\u{EE00}" + hiraganaToKatakana(reading) + "\u{EE01}"
        return prompt
    }

    // MARK: - llama.cpp

    private func ensureLoaded() throws {
        guard runtime == nil else { return }
        guard FileManager.default.fileExists(atPath: modelPath) else {
            throw ConversionError.modelNotFound(modelPath)
        }

        llama_backend_init()
        var modelParams = llama_model_default_params()
        // デフォルトは全レイヤをMetalへ。IROHA_GPU_LAYERS=0 でCPU推論に切替可能（計測用）
        if let env = ProcessInfo.processInfo.environment["IROHA_GPU_LAYERS"],
           let layers = Int32(env) {
            modelParams.n_gpu_layers = layers
        } else {
            modelParams.n_gpu_layers = 99
        }

        guard let loadedModel = llama_model_load_from_file(modelPath, modelParams) else {
            throw ConversionError.modelLoadFailed(modelPath)
        }

        var contextParams = llama_context_default_params()
        contextParams.n_ctx = 512
        contextParams.n_batch = 512

        guard let createdContext = llama_init_from_model(loadedModel, contextParams) else {
            llama_model_free(loadedModel)
            throw ConversionError.modelLoadFailed("コンテキストの作成に失敗")
        }

        guard let vocab = llama_model_get_vocab(loadedModel) else {
            llama_free(createdContext)
            llama_model_free(loadedModel)
            throw ConversionError.modelLoadFailed("vocabの取得に失敗")
        }

        guard let chain = llama_sampler_chain_init(llama_sampler_chain_default_params()) else {
            llama_free(createdContext)
            llama_model_free(loadedModel)
            throw ConversionError.modelLoadFailed("サンプラの作成に失敗")
        }
        llama_sampler_chain_add(chain, llama_sampler_init_greedy())

        self.runtime = Runtime(model: loadedModel, context: createdContext, vocab: vocab, sampler: chain)
    }

    private func tokenize(_ text: String, addSpecial: Bool) throws -> [llama_token] {
        guard let vocab = runtime?.vocab else { throw ConversionError.modelLoadFailed("vocab未初期化") }
        let utf8 = Array(text.utf8)
        var tokens = [llama_token](repeating: 0, count: utf8.count + 8)
        let count = utf8.withUnsafeBufferPointer { buffer in
            buffer.baseAddress!.withMemoryRebound(to: CChar.self, capacity: utf8.count) { pointer in
                llama_tokenize(vocab, pointer, Int32(utf8.count), &tokens, Int32(tokens.count), addSpecial, true)
            }
        }
        guard count >= 0 else { throw ConversionError.inferenceFailed("トークン化に失敗") }
        tokens.removeLast(tokens.count - Int(count))
        return tokens
    }

    private func decode(ctx: OpaquePointer, tokens: inout [llama_token]) throws {
        let result = tokens.withUnsafeMutableBufferPointer { buffer in
            llama_decode(ctx, llama_batch_get_one(buffer.baseAddress, Int32(buffer.count)))
        }
        guard result == 0 else { throw ConversionError.inferenceFailed("llama_decode=\(result)") }
    }

    private func decode(ctx: OpaquePointer, tokens token: inout llama_token) throws {
        let result = withUnsafeMutablePointer(to: &token) { pointer in
            llama_decode(ctx, llama_batch_get_one(pointer, 1))
        }
        guard result == 0 else { throw ConversionError.inferenceFailed("llama_decode=\(result)") }
    }
}
