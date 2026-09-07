import CLlama
import Foundation

/// zenz-v3（GPT-2系かな漢字変換モデル）をllama.cppで動かす変換エンジン。
///
/// プロンプト形式（zenz-v3）:
///   [U+EE02 + 左文脈] + U+EE00 + カタカナ読み + U+EE01 → 変換結果
public actor ZenzEngine: ConversionEngine {

    /// 既定のモデルの場所（`DataDirectory` の設定に追随する）
    public static var defaultModelPath: String { DataDirectory.defaultModelURL.path }

    /// llama.cppのリソース一式。deinitで確実に解放する
    private final class Runtime: @unchecked Sendable {
        let model: OpaquePointer
        let context: OpaquePointer
        let vocab: OpaquePointer
        /// トークンID → 出力文字列（UTF-8として不正なバイト断片はnil）
        let tokenTexts: [String?]
        /// トークンID → 生成終了とみなすトークンか（EOG・zenzの特殊トークン）
        let tokenIsTerminator: [Bool]

        init(model: OpaquePointer, context: OpaquePointer, vocab: OpaquePointer,
             tokenTexts: [String?], tokenIsTerminator: [Bool]) {
            self.model = model
            self.context = context
            self.vocab = vocab
            self.tokenTexts = tokenTexts
            self.tokenIsTerminator = tokenIsTerminator
        }

        deinit {
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
                                      forcedFirstToken: nil, reading: reading).text
            return [result.isEmpty ? reading : result]
        }

        // n-best: 先頭トークンを上位候補に分岐し、それぞれ貪欲に補完する。
        // かな漢字変換では先頭の文字が同音異義語をほぼ決めるため、これで多様な候補が得られる。
        // 各候補には系列全体の対数確率を付け、最良候補から `nbestLogProbWindow` 以上
        // 離れたものは捨てる（読み制約は漢字・英字の読みを検証できないので、
        // 「活用を」「NIPPON」のような読みの合わない候補が探索の埋め草として通ってしまう。
        // モデル自身の確からしさで足切りすることでそれを除く）
        var tokens = promptTokens
        llama_memory_clear(llama_get_memory(runtime.context), true)
        try decode(ctx: runtime.context, tokens: &tokens)

        let constraint = ReadingConstraint(reading: reading)
        let firstTokens = topTokens(runtime: runtime, count: candidateCount * 8)
        var scored: [(text: String, logProb: Float)] = []
        var bestLogProb = -Float.infinity
        for first in firstTokens {
            try Task.checkCancellation()
            // 先頭トークンの確率だけで既に足切り線を下回るなら、続きを生成しても届かない
            // （後続の対数確率は0以下）。先頭トークンは確率の高い順なので以降も同様
            if first.logProb < bestLogProb - Self.nbestLogProbWindow { break }
            if llama_vocab_is_eog(runtime.vocab, first.token) { continue }
            // 読みと辻褄の合わない先頭トークンは候補にしない
            if let constraint {
                guard let text = runtime.tokenTexts[Int(first.token)],
                      constraint.advance(constraint.initialMask, text: text) != 0 else { continue }
            }
            let generated = try generate(runtime: runtime, promptTokens: promptTokens,
                                         forcedFirstToken: first, reading: reading)
            guard !generated.text.isEmpty else { continue }
            if let index = scored.firstIndex(where: { $0.text == generated.text }) {
                // 同じ文字列に別のトークン列で到達した: 確率の高い方を残す
                scored[index].logProb = max(scored[index].logProb, generated.logProb)
            } else {
                scored.append(generated)
            }
            bestLogProb = max(bestLogProb, generated.logProb)
            if scored.count >= candidateCount { break }
        }
        let results = scored
            .filter { $0.logProb >= bestLogProb - Self.nbestLogProbWindow }
            .sorted { $0.logProb > $1.logProb }
            .map(\.text)
        return results.isEmpty ? [reading] : results
    }

    /// n-best候補を残す対数確率の幅（最良候補との差、nat単位）。
    /// 実測では同音異義語どうしの差は6nat以内に収まる（きしゃ: 記者 −1.3 〜 樹舎 −5.9）。
    /// 文脈で確信が高いときは読みの合わない候補が大きく離れる
    /// （ガイドする＋ないようを: 内容を −0.0、活用を −16.4、NIPPON −18.4）
    static let nbestLogProbWindow: Float = 8

    /// プロンプト評価直後のlogitsから上位トークンを対数確率つきで返す
    private func topTokens(runtime: Runtime, count: Int) -> [(token: llama_token, logProb: Float)] {
        guard let logits = llama_get_logits_ith(runtime.context, -1) else { return [] }
        let vocabSize = Int(llama_vocab_n_tokens(runtime.vocab))
        let logZ = Self.logSumExp(logits, count: vocabSize)
        var indexed: [(token: llama_token, logit: Float)] = []
        indexed.reserveCapacity(vocabSize)
        for index in 0..<vocabSize {
            indexed.append((llama_token(index), logits[index]))
        }
        return indexed.sorted { $0.logit > $1.logit }.prefix(count)
            .map { (token: $0.token, logProb: $0.logit - logZ) }
    }

    /// log Σ exp(logits)（logitを対数確率に直す正規化項）
    static func logSumExp(_ logits: UnsafeMutablePointer<Float>, count: Int) -> Float {
        var maxLogit = -Float.infinity
        for index in 0..<count { maxLogit = max(maxLogit, logits[index]) }
        var sum: Float = 0
        for index in 0..<count { sum += expf(logits[index] - maxLogit) }
        return maxLogit + logf(sum)
    }

    /// 現在のlogitsから、読みの制約を満たすもっとも尤度の高いトークンを選ぶ。
    /// 制約を満たすトークンが1つもなければ制約を諦めて素の最尤トークンを返す（relaxed）
    private func selectToken(runtime: Runtime, constraint: ReadingConstraint?, mask: UInt64)
        -> (token: llama_token, mask: UInt64, relaxed: Bool, logProb: Float)? {
        guard let logits = llama_get_logits_ith(runtime.context, -1) else { return nil }
        let vocabSize = Int(llama_vocab_n_tokens(runtime.vocab))
        let logZ = Self.logSumExp(logits, count: vocabSize)
        var best: (index: Int, logit: Float)?
        var bestMask: UInt64 = 0
        var fallback: (index: Int, logit: Float)?

        for index in 0..<vocabSize {
            let logit = logits[index]
            if fallback == nil || logit > fallback!.logit { fallback = (index, logit) }
            guard let constraint else { continue }
            // 現在の最良より低いトークンは制約を調べるまでもない
            if let best, logit <= best.logit { continue }
            if runtime.tokenIsTerminator[index] {
                // 読みを使い切っていなければ終端は許さない（食い残し防止）
                guard constraint.isComplete(mask) else { continue }
                best = (index, logit)
                bestMask = mask
            } else {
                guard let text = runtime.tokenTexts[index] else { continue }
                let next = constraint.advance(mask, text: text)
                guard next != 0 else { continue }
                best = (index, logit)
                bestMask = next
            }
        }

        if let best, constraint != nil {
            return (llama_token(best.index), bestMask, false, best.logit - logZ)
        }
        guard let fallback else { return nil }
        return (llama_token(fallback.index), 0, constraint != nil, fallback.logit - logZ)
    }

    /// 貪欲法で1候補を生成する。forcedFirstTokenがあれば先頭をそのトークンに固定する。
    /// 各ステップでは読みと辻褄の合うトークンだけを選ぶ（constrained decoding）。
    /// - Returns: 生成した文字列と、先頭トークン・各トークン・終端まで含めた系列の対数確率
    private func generate(
        runtime: Runtime,
        promptTokens: [llama_token],
        forcedFirstToken: (token: llama_token, logProb: Float)?,
        reading: String
    ) throws -> (text: String, logProb: Float) {
        let ctx = runtime.context
        let vocab = runtime.vocab
        var tokens = promptTokens
        var logProb: Float = 0
        if let forcedFirstToken {
            tokens.append(forcedFirstToken.token)
            logProb += forcedFirstToken.logProb
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
        if let forcedFirstToken { appendPiece(forcedFirstToken.token) }

        // 読みの消費状況（constrained decoding用）。追跡できない読みや
        // 制約を満たすトークンが尽きた場合はnilにして素の貪欲生成に戻す
        var constraint = ReadingConstraint(reading: reading)
        var mask = constraint?.initialMask ?? 0
        if let forcedFirstToken, let active = constraint {
            mask = active.advance(mask, text: runtime.tokenTexts[Int(forcedFirstToken.token)] ?? "")
            if mask == 0 { constraint = nil }
        }

        // 生成上限: 読みの長さに比例させつつ、KVキャッシュ（n_ctx）を超えて
        // llama_decodeが失敗しないように残り容量で抑える
        let remainingContext = Int(llama_n_ctx(ctx)) - tokens.count
        let maxNewTokens = max(0, min(reading.count * 3 + 8, remainingContext))
        generation: for _ in 0..<maxNewTokens {
            try Task.checkCancellation()
            guard let picked = selectToken(runtime: runtime, constraint: constraint, mask: mask) else { break }
            let token = picked.token
            if picked.relaxed { constraint = nil }
            mask = picked.mask
            logProb += picked.logProb
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

        let text = Self.decodeUTF8DroppingFragments(outputBytes)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return (text, logProb)
    }

    /// 生成バイト列をUTF-8として文字列化する。
    ///
    /// 制約なし生成ではバイト単位のトークンが選ばれることがあり、多バイト文字の途中で
    /// 終端や生成上限に達すると末尾に不完全なバイト列が残る。これをエラーにすると
    /// ライブ変換の表示が更新されず古い結果のまま止まるため、断片は捨てて返す
    static func decodeUTF8DroppingFragments(_ data: Data) -> String {
        var bytes = data
        // 末尾の断片は最大3バイト（4バイト文字の先頭3バイト）
        for _ in 0..<4 {
            if let text = String(data: bytes, encoding: .utf8) { return text }
            guard !bytes.isEmpty else { break }
            bytes.removeLast()
        }
        // 途中にも不正なバイトがある: 置換文字に落としてから取り除く
        return String(decoding: data, as: UTF8.self).replacingOccurrences(of: "\u{FFFD}", with: "")
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

        let (tokenTexts, tokenIsTerminator) = Self.buildTokenTable(vocab: vocab)

        self.runtime = Runtime(model: loadedModel, context: createdContext, vocab: vocab,
                               tokenTexts: tokenTexts, tokenIsTerminator: tokenIsTerminator)
    }

    /// 語彙全体の出力文字列を1度だけ取り出しておく（制約判定を毎トークン安く行うため）
    private static func buildTokenTable(vocab: OpaquePointer) -> (texts: [String?], terminators: [Bool]) {
        let vocabSize = Int(llama_vocab_n_tokens(vocab))
        var texts = [String?](repeating: nil, count: vocabSize)
        var terminators = [Bool](repeating: false, count: vocabSize)
        var buffer = [CChar](repeating: 0, count: 128)
        for index in 0..<vocabSize {
            let token = llama_token(index)
            if llama_vocab_is_eog(vocab, token) {
                terminators[index] = true
                continue
            }
            let written = llama_token_to_piece(vocab, token, &buffer, Int32(buffer.count), 0, true)
            guard written > 0 else { continue }
            let bytes = buffer.withUnsafeBytes { raw in
                Data(raw.bindMemory(to: UInt8.self).prefix(Int(written)))
            }
            guard let text = String(data: bytes, encoding: .utf8), !text.isEmpty else { continue }
            // zenzの特殊トークン（私用領域 U+EE00-U+EE0F）は生成終了の印
            if text.unicodeScalars.contains(where: { (0xEE00...0xEE0F).contains($0.value) }) {
                terminators[index] = true
                continue
            }
            texts[index] = text
        }
        return (texts, terminators)
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
