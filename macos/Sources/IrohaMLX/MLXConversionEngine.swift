import Foundation
import MLX
import MLXNN
import IrohaCore
import IrohaTrain

/// かな漢字変換エンジンの MLX 版（`ZenzEngine` の推論部分だけを MLX に置き換えたもの）。対応は T5 のみ。
///
/// プロンプト・読み制約・終端・n-best・一括採点（`score`）の規則は `ZenzEngine` と同じにしてあり、
/// 出力も同じになる（2026-10-01、T5 e12d2 最終で AJIMEE 200 問 × 2 条件がすべて一致。
/// `training/t5/MAC-AJIMEE-MLX-2026-10-01.md`）。違うのは計算の仕組みだけ:
///
/// - 重み・順伝播は `IrohaTrain` の `T5Model`（追加学習と同じ実装。llama.cpp とのロジット一致は `T5ParityTests`）
/// - 1 ステップの手間が llama.cpp より小さいので、デコーダが 2 層の T5 では速い（1 回の変換で約 2 割。
///   `training/t5/MAC-SPEED-MLX-vs-LLAMA-2026-10-01.md`）。12 層の zenz（GPT-2）では逆に遅いので対応していない
/// - 計算は bf16。T5 は f16 だと途中の値が溢れて出力が壊れる（bf16 は f32 と同じ範囲を持つ）。
///   計測用に `IROHA_MLX_DTYPE=f32` で f32 にできる
/// - 量子化済みの GGUF（Q5_K_M など）は f16 に戻した版（`ModelRequantizer.ensureF16`、キャッシュ）を読む
/// - LoRA アダプタ（llama.cpp 形式の GGUF）は読み込み時に重みへ足し込む（`LoRAAdapterMerger`）
public actor MLXConversionEngine: ConversionEngine, CandidateScorer {

    /// このエンジンが読めるアーキテクチャ（`general.architecture`）
    public static let supportedArchitectures = [T5Model.architecture]

    /// 一括採点で 1 回に流す候補数の上限（`ZenzEngine.maxScoringSequences` と同じ）
    static let maxScoringSequences = 32

    /// 変換が終わった時点で、MLX が使い回し用に手元に残しているメモリがこれを超えていたら手放す（`trimCache`）
    static let cacheWatermarkBytes = 128 * 1024 * 1024

    public let modelPath: String
    public let adapterPath: String?
    private let restrictLatinToReading: Bool
    private let usesReadingConstraint: Bool
    private let dtype: DType

    private struct Runtime {
        let model: T5Model
        let tokenizer: VocabTokenizer
        /// 出力文字列（出さないトークンは nil）と終端トークン（`ZenzEngine.buildTokenTable` と同じ）
        let tokenTexts: [String?]
        let tokenIsTerminator: [Bool]
        let terminatorIndices: MLXArray
        let eos: Int32
        let decoderStart: Int32
    }

    private var runtime: Runtime?
    /// デコーダの自己注意のバイアス（相対位置 + 因果マスク）[1, H, L, L]（長い読みが来たら作り直す）
    private var decoderBias: MLXArray?

    /// - Parameter usesFloat32: 真なら f32 で計算する（`ZenzEngine` との突き合わせ用。既定は bf16、
    ///   環境変数 `IROHA_MLX_DTYPE=f32` でも f32 になる）
    public init(modelPath: String, adapterPath: String? = nil,
                restrictLatinToReading: Bool = false, usesReadingConstraint: Bool = true, usesFloat32: Bool = false) {
        self.modelPath = modelPath
        self.adapterPath = (adapterPath?.isEmpty == false) ? adapterPath : nil
        self.restrictLatinToReading = restrictLatinToReading
        self.usesReadingConstraint = usesReadingConstraint
        let float32 = usesFloat32 || ProcessInfo.processInfo.environment["IROHA_MLX_DTYPE"] == "f32"
        dtype = float32 ? .float32 : .bfloat16
    }

    /// `path` のモデルをこのエンジンで読めるか（アーキテクチャだけを見る。ファイルが無ければ偽）
    public static func supports(modelPath path: String) -> Bool {
        guard let gguf = try? GGUFFile(path: path), let architecture = gguf.architecture else { return false }
        return supportedArchitectures.contains(architecture)
    }

    public func prewarm() throws {
        try ensureLoaded()
    }

    // MARK: - 読み込み

    private func ensureLoaded() throws {
        guard runtime == nil else { return }
        try InferenceTimer.measureModelLoad { try load() }
    }

    private func load() throws {
        guard FileManager.default.fileExists(atPath: modelPath) else {
            throw ConversionError.modelNotFound(modelPath)
        }
        let architecture: String?
        do {
            architecture = try GGUFFile(path: modelPath).architecture
        } catch {
            throw ConversionError.modelLoadFailed("\(modelPath): \(error)")
        }
        guard let architecture, Self.supportedArchitectures.contains(architecture) else {
            throw ConversionError.modelLoadFailed(
                "MLX のエンジンは T5 のモデルにだけ対応しています（このモデル: \(architecture ?? "不明")）")
        }
        if let adapterPath, !FileManager.default.fileExists(atPath: adapterPath) {
            throw ConversionError.modelNotFound(adapterPath)
        }
        do {
            // 量子化済みなら f16 に戻した版（既に f16/f32 ならそのまま）
            let path = try ModelRequantizer.ensureF16(basePath: modelPath).path
            let gguf = try GGUFFile(path: path)
            let adapter = try adapterPath.map { try LoRAAdapterMerger(path: $0, baseArchitecture: architecture) }
            let model = try T5Model(gguf: gguf, lora: nil, adapter: adapter, dtype: dtype)
            // 読み込みの途中で使ったメモリを使い回し用に残さない
            Memory.clearCache()
            let tokenizer = try VocabTokenizer(modelPath: path)
            let table = tokenizer.generationTable()
            runtime = Runtime(
                model: model, tokenizer: tokenizer, tokenTexts: table.texts, tokenIsTerminator: table.terminators,
                terminatorIndices: MLXArray(table.terminators.indices.filter { table.terminators[$0] }.map(Int32.init)),
                eos: tokenizer.eos, decoderStart: model.decoderStartToken)
        } catch let error as ConversionError {
            throw error
        } catch {
            throw ConversionError.modelLoadFailed("\(modelPath): \(error)")
        }
    }

    // MARK: - ConversionEngine

    public func convert(reading: String, context leftContext: String, candidateCount: Int) async throws -> [String] {
        try ensureLoaded()
        guard let runtime else { throw ConversionError.modelLoadFailed("内部状態が不正です") }
        defer { trimCache() }
        return try InferenceTimer.measureNeuralNetwork {
            try convert(runtime: runtime, reading: reading, leftContext: leftContext, candidateCount: candidateCount)
        }
    }

    /// `ZenzEngine.convert` と同じ: 1 候補なら貪欲生成、複数なら先頭トークンを上位に分岐して貪欲に補完し、
    /// 系列全体の対数確率で並べる（最良から `nbestLogProbWindow` 以上離れた候補は捨てる）
    private func convert(runtime: Runtime, reading: String, leftContext: String, candidateCount: Int) throws -> [String] {
        let encoded = encode(runtime: runtime, reading: reading, leftContext: leftContext)
        let maxNew = reading.count * 3 + 8
        // 開始トークン + 先頭の固定（n-best）+ 生成 maxNew。64 単位に切り上げて形をそろえる
        var state = runtime.model.startDecoding(memory: encoded.memory, sourceMask: encoded.mask,
                                                capacity: Self.bucket(maxNew + 2, step: 64))
        let firstLogits = step(runtime: runtime, state: &state, token: runtime.decoderStart)

        if candidateCount <= 1 {
            let text = try generate(runtime: runtime, state: state, logits: firstLogits,
                                    forcedFirst: nil, reading: reading, maxNew: maxNew).text
            return [text.isEmpty ? reading : text]
        }

        let constraint = makeReadingConstraint(reading: reading)
        let logZ = Self.logSumExp(firstLogits)
        let ranked: [Int] = firstLogits.indices.sorted { firstLogits[$0] > firstLogits[$1] }
        let firstTokens: [(token: Int, logProb: Float)] = ranked.prefix(candidateCount * 8).map { index in
            (token: index, logProb: firstLogits[index] - logZ)
        }
        var scored: [(text: String, logProb: Float)] = []
        var bestLogProb = -Float.infinity
        for first in firstTokens {
            try Task.checkCancellation()
            if first.logProb < bestLogProb - ZenzEngine.nbestLogProbWindow { break }
            if first.token < runtime.tokenIsTerminator.count, runtime.tokenIsTerminator[first.token] { continue }
            if let constraint {
                guard first.token < runtime.tokenTexts.count, let text = runtime.tokenTexts[first.token],
                      constraint.advance(constraint.initialMask, text: text) != 0 else { continue }
            }
            let generated = try generate(runtime: runtime, state: state, logits: firstLogits,
                                         forcedFirst: first, reading: reading, maxNew: maxNew)
            guard !generated.text.isEmpty else { continue }
            if let index = scored.firstIndex(where: { $0.text == generated.text }) {
                scored[index].logProb = max(scored[index].logProb, generated.logProb)
            } else {
                scored.append((generated.text, generated.logProb))
            }
            bestLogProb = max(bestLogProb, generated.logProb)
            if scored.count >= candidateCount { break }
        }
        let results = scored
            .filter { $0.logProb >= bestLogProb - ZenzEngine.nbestLogProbWindow }
            .sorted { $0.logProb > $1.logProb }
            .map(\.text)
        return results.isEmpty ? [reading] : results
    }

    // MARK: - メモリ

    /// MLX が使い回し用に手元に残しているメモリが `cacheWatermarkBytes` を超えていたら手放す。
    ///
    /// MLX は解放したメモリをほぼ同じ大きさの要求にだけ使い回す。遅延評価で 1 回の計算の途中の配列が
    /// 同時に生きるうえ、入力の長さの刻みごとに大きさが違うので、放っておくと使い回し用の領域が
    /// 1GB 近くまで膨らむ（2026-10-01、AJIMEE 200 問・辞書ラティスで第一候補を決める条件）。
    /// 逆に `Memory.cacheLimit` で小さく縛ると、上限が読み込みや採点の大きな配列で埋まって
    /// 生成の小さな配列が使い回されなくなり、llama.cpp より遅くなる。変換の切れ目でまとめて手放すのが
    /// 速さとメモリの釣り合いが良い（128MB で生成だけの変換は 24.4ms。上限なし 23.8ms、llama.cpp 30.6ms）
    private func trimCache() {
        if Memory.cacheMemory > Self.cacheWatermarkBytes { Memory.clearCache() }
    }

    // MARK: - 生成

    private func promptTokens(runtime: Runtime, reading: String, leftContext: String) -> [Int32] {
        let prompt = ZenzEngine.buildPrompt(reading: reading, leftContext: leftContext,
                                            maxContextLength: LeftContext.maxLength)
        // `ZenzEngine.tokenizePrompt` と同じ: 特殊トークンを自動で付けず末尾に </s>
        return runtime.tokenizer.tokenize(prompt, addSpecial: false) + [runtime.eos]
    }

    /// プロンプトをエンコードしたエンコーダ出力 [1, S, D] と有効位置のマスク [1, S]。
    /// 入力は 16 の倍数にパディングする（形をそろえて MLX のメモリを使い回すため。`T5Model.DecoderState` 参照）。
    /// パディングはエンコーダの自己注意と交差注意の両方でマスクされ、有効位置の値は変わらない
    private func encode(runtime: Runtime, reading: String, leftContext: String) -> (memory: MLXArray, mask: MLXArray) {
        let source = promptTokens(runtime: runtime, reading: reading, leftContext: leftContext)
        let length = Self.bucket(source.count, step: 16)
        let padded = source + [Int32](repeating: 0, count: length - source.count)
        let valid = [Float](repeating: 1, count: source.count) + [Float](repeating: 0, count: length - source.count)
        let mask = MLXArray(valid, [1, length]).asType(dtype)
        return (runtime.model.encode(MLXArray(padded, [1, length]), sourceMask: mask), mask)
    }

    /// `value` 以上で最小の `step` の倍数
    static func bucket(_ value: Int, step: Int) -> Int {
        (value + step - 1) / step * step
    }

    /// `value` 以上で最小の 2 の累乗
    static func powerOfTwo(atLeast value: Int) -> Int {
        var size = 1
        while size < value { size *= 2 }
        return size
    }

    /// 自己注意のバイアス（相対位置 + 因果マスク）を `length` 位置ぶん以上用意する（作り置きを使い回す）
    private func decoderBias(runtime: Runtime, length: Int) -> MLXArray {
        if let decoderBias, decoderBias.dim(2) >= length { return decoderBias }
        let bias = runtime.model.decoderSelfBias(length: Self.bucket(max(length, 128), step: 64))
        eval(bias)
        decoderBias = bias
        return bias
    }

    /// デコーダに 1 トークン入れて、次のトークンのロジット（CPU 上の Float 配列）を返す
    private func step(runtime: Runtime, state: inout T5Model.DecoderState, token: Int32) -> [Float] {
        let bias = decoderBias(runtime: runtime, length: state.capacity)
        let logits = runtime.model.decodeStep(token, state: &state, selfBias: bias).asType(.float32)
        eval([logits] + state.arrays)
        return logits.asArray(Float.self)
    }

    /// 貪欲法で 1 候補を生成する（`ZenzEngine.generate` と同じ）。`logits` は `state` の直後のロジット。
    /// `forcedFirst` があれば先頭をそのトークンに固定する
    private func generate(runtime: Runtime, state initial: T5Model.DecoderState, logits initialLogits: [Float],
                          forcedFirst: (token: Int, logProb: Float)?, reading: String,
                          maxNew: Int) throws -> (text: String, logProb: Float) {
        var state = initial
        var logits = initialLogits
        var outputBytes: [UInt8] = []
        var logProb: Float = 0
        var constraint = makeReadingConstraint(reading: reading)
        var mask = constraint?.initialMask ?? 0
        if let forcedFirst {
            outputBytes += runtime.tokenizer.pieceBytes(Int32(forcedFirst.token))
            logProb += forcedFirst.logProb
            if let active = constraint {
                mask = active.advance(mask, text: runtime.tokenTexts[forcedFirst.token] ?? "")
                if mask == 0 { constraint = nil }
            }
            logits = step(runtime: runtime, state: &state, token: Int32(forcedFirst.token))
        }
        // 上限は先頭の固定とは別に `maxNew` 回（`ZenzEngine.generate` と同じ）
        for iteration in 0..<maxNew {
            try Task.checkCancellation()
            guard let picked = selectToken(runtime: runtime, logits: logits, constraint: constraint, mask: mask) else {
                break
            }
            if picked.relaxed { constraint = nil }
            mask = picked.mask
            logProb += picked.logProb
            if runtime.tokenIsTerminator[picked.token] { break }
            outputBytes += runtime.tokenizer.pieceBytes(Int32(picked.token))
            // 最後の 1 回は次のロジットが要らない
            guard iteration < maxNew - 1 else { break }
            logits = step(runtime: runtime, state: &state, token: Int32(picked.token))
        }
        let text = ZenzEngine.decodeUTF8DroppingFragments(Data(outputBytes))
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return (text, logProb)
    }

    /// 読みと辻褄の合うトークンのうち最もロジットの高いものを選ぶ（`ZenzEngine.selectToken` と同じ規則）。
    /// 制約を満たすトークンが無ければ制約を外して最大のものを選び、`relaxed` を真にする
    private func selectToken(runtime: Runtime, logits: [Float], constraint: ReadingConstraint?, mask: UInt64)
        -> (token: Int, mask: UInt64, relaxed: Bool, logProb: Float)? {
        let vocabSize = min(logits.count, runtime.tokenTexts.count)
        let logZ = Self.logSumExp(logits)
        var best: (index: Int, logit: Float)?
        var second: Float = -.infinity
        var bestMask: UInt64 = 0
        var fallback: (index: Int, logit: Float)?
        for index in 0..<vocabSize {
            let logit = logits[index]
            if fallback == nil || logit > fallback!.logit { fallback = (index, logit) }
            guard let constraint else { continue }
            if logit <= second { continue }
            let nextMask: UInt64
            if runtime.tokenIsTerminator[index] {
                guard constraint.isComplete(mask) else { continue }
                nextMask = mask
            } else {
                guard let text = runtime.tokenTexts[index] else { continue }
                nextMask = constraint.advance(mask, text: text)
                guard nextMask != 0 else { continue }
            }
            if let current = best, logit <= current.logit {
                second = logit
            } else {
                second = best?.logit ?? -.infinity
                best = (index, logit)
                bestMask = nextMask
            }
        }
        if let best, constraint != nil { return (best.index, bestMask, false, best.logit - logZ) }
        guard let fallback else { return nil }
        return (fallback.index, 0, constraint != nil, fallback.logit - logZ)
    }

    private func makeReadingConstraint(reading: String) -> ReadingConstraint? {
        guard usesReadingConstraint else { return nil }
        return ReadingConstraint(reading: reading, restrictLatinToReading: restrictLatinToReading)
    }

    static func logSumExp(_ logits: [Float]) -> Float {
        guard let maxLogit = logits.max(), maxLogit > -.infinity else { return -.infinity }
        var sum: Float = 0
        for logit in logits { sum += expf(logit - maxLogit) }
        return maxLogit + logf(sum)
    }

    // MARK: - CandidateScorer

    /// 候補それぞれの log P(候補 + 終端 | プロンプト)（`ZenzEngine.score` と同じ定義。終端は終端トークン群の合計確率）。
    /// エンコードは 1 回だけ行い、候補をデコーダに並べて一度に流す（teacher forcing）
    public func score(candidates: [String], reading: String, context leftContext: String) async throws -> [Float] {
        try ensureLoaded()
        guard let runtime, !candidates.isEmpty else { return [] }
        defer { trimCache() }
        return try InferenceTimer.measureNeuralNetwork {
            try score(runtime: runtime, candidates: candidates, reading: reading, leftContext: leftContext)
        }
    }

    private func score(runtime: Runtime, candidates: [String], reading: String, leftContext: String) throws -> [Float] {
        let (memory, sourceMask) = encode(runtime: runtime, reading: reading, leftContext: leftContext)
        let tokenized = candidates.map { runtime.tokenizer.tokenize($0, addSpecial: false) }
        var scores = [Float](repeating: -.infinity, count: candidates.count)
        // 空文字列は採点不能（-inf のまま。`ZenzEngine` と同じ）
        let valid = tokenized.indices.filter { !tokenized[$0].isEmpty }
        var start = 0
        while start < valid.count {
            try Task.checkCancellation()
            let chunk = Array(valid[start ..< min(start + Self.maxScoringSequences, valid.count)])
            let chunkScores = scoreChunk(runtime: runtime, memory: memory, sourceMask: sourceMask,
                                         sequences: chunk.map { tokenized[$0] })
            for (offset, index) in chunk.enumerated() { scores[index] = chunkScores[offset] }
            start += chunk.count
        }
        return scores
    }

    private func scoreChunk(runtime: Runtime, memory: MLXArray, sourceMask: MLXArray, sequences: [[Int32]]) -> [Float] {
        // 形をそろえる（候補数は 2 の累乗、長さは 16 の倍数。`encode` と同じ理由）。増えた行・列はパディングで、
        // デコーダは因果的なので後ろの列は前の位置の値に影響しない
        let batch = Self.powerOfTwo(atLeast: sequences.count)
        // デコーダ入力 = 開始トークン + 候補。位置 j が位置 j の候補トークン（最後は終端）を予測する
        let length = Self.bucket(sequences.map(\.count).max()! + 1, step: 16)
        var inputs = [Int32](repeating: 0, count: batch * length)
        // 出力層に通す位置（行 × 長さを平らにした番号）と、その位置で予測するトークン（終端の位置は 0。使わない）
        var positions: [Int32] = []
        var targets: [Int32] = []
        for (row, sequence) in sequences.enumerated() {
            inputs[row * length] = runtime.decoderStart
            for (offset, token) in sequence.enumerated() {
                inputs[row * length + offset + 1] = token
            }
            for offset in 0 ... sequence.count {
                positions.append(Int32(row * length + offset))
                targets.append(offset < sequence.count ? sequence[offset] : 0)
            }
        }
        // 出力層（語彙 × 位置の大きな配列）は使う位置だけで計算する。位置の数も 64 単位に切り上げる
        let used = positions.count
        let rows = Self.bucket(used, step: 64)
        positions += [Int32](repeating: 0, count: rows - used)
        targets += [Int32](repeating: 0, count: rows - used)

        let hidden = runtime.model.decodeHidden(
            MLXArray(inputs, [batch, length]),
            memory: broadcast(memory, to: [batch, memory.dim(1), memory.dim(2)]),
            sourceMask: broadcast(sourceMask, to: [batch, sourceMask.dim(1)]))
        let selected = hidden.reshaped(batch * length, -1).take(MLXArray(positions), axis: 0)
        let logits = runtime.model.outputLogits(selected).asType(.float32)  // [rows, V]
        let logZ = MLX.logSumExp(logits, axis: -1)
        let picked = takeAlong(logits, MLXArray(targets, [rows, 1]), axis: -1).squeezed(axis: -1) - logZ
        let terminal = MLX.logSumExp(logits.take(runtime.terminatorIndices, axis: -1), axis: -1) - logZ
        eval(picked, terminal)
        let pickedValues = picked.asArray(Float.self)
        let terminalValues = terminal.asArray(Float.self)
        var scores: [Float] = []
        var index = 0
        for sequence in sequences {
            var total: Float = 0
            for _ in sequence.indices {
                total += pickedValues[index]
                index += 1
            }
            scores.append(total + terminalValues[index])
            index += 1
        }
        return scores
    }
}
