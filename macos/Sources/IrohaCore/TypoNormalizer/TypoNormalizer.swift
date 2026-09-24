import Foundation

/// 打ち間違いの訂正案。`margin` が大きいほどモデルが「入力のままより訂正のほうがありそう」と見ている
public struct TypoCorrection: Sendable, Equatable {
    /// 入力された読み（ひらがな）
    public let reading: String
    /// 訂正した読み（ひらがな）
    public let corrected: String
    /// logP(訂正 | 入力) − logP(入力そのまま | 入力)
    public let margin: Double

    public init(reading: String, corrected: String, margin: Double) {
        self.reading = reading
        self.corrected = corrected
        self.margin = margin
    }
}

/// ローマ字入力の打ち間違いを、かな漢字変換の**手前**で直す「読み → 読み」のモデル。
///
/// ```text
/// 打鍵 → ローマ字→かな変換 → [ TypoNormalizer ] → 辞書ラティス → かな漢字変換NN
/// ```
///
/// 3.2M パラメータの文字単位 Transformer encoder–decoder。学習と書き出しは
/// `training/typo-normalizer/`、移植の仕様は同ディレクトリの SWIFT-PORT.md が正。
///
/// ## 使いかたの制約（SWIFT-PORT.md §5・§9）
///
/// - **毎打鍵では呼ばない。** 打ち終わった読みだけで学習しているので、入力途中の
///   「こんにち」のような未完成の読みを typo と誤認する。変換を要求された時点で 1 回だけ
/// - **`ConversionEngine` のデコレータ鎖には入れない。** ユーザ定義の変換ルールと同じく、
///   独立した候補生成源として候補ウィンドウに合流させる（過剰訂正の実害を「候補が 1 個増える」で止める）
/// - 読みが 48 文字を超えるもの・語彙に無い文字を含むものには使わない（`supports(reading:)`）
/// - ユーザ辞書や学習の結果には使わない（このモデルはコーパスの読み分布しか知らない）
///
/// モデルの読み込みは最初の呼び出しまで遅らせる（設定でOFFなら一生読まない）。
/// モデル自体もアプリに同梱せず、設定でONにしたときに取得する（`TypoNormalizerCatalog`）。
public actor TypoNormalizer {

    /// 訂正を採用するしきい値の既定値。SWIFT-PORT.md §4 の推奨
    /// （候補ウィンドウに出す設計なら 1.5〜2.0、黙って置き換えるなら 4.0）。
    /// test で選んだ値なので、実験側が valid で選び直したら追随すること
    public static let defaultThreshold = 2.0

    /// 学習データの読みの上限。これより長い読みには使わない
    public static let maxReadingLength = 48

    public enum LoadError: Error, CustomStringConvertible {
        case notFound(URL)

        public var description: String {
            switch self {
            case .notFound(let url): return "Typo Normalizer のモデルがありません: \(url.path)"
            }
        }
    }

    private let directory: URL
    private var runtime: Runtime?
    private var loadFailure: (any Error)?

    public init(directory: URL) {
        self.directory = directory
    }

    /// モデルの置き場所。**アプリには同梱せず、設定でONにしたときにダウンロードする**
    /// （重みは本体コード(MIT)と別ライセンスなので、配布物を分けておくほうが扱いやすい）。
    ///
    /// 探す順は 環境変数 → `<データフォルダ>/models/typo-normalizer/` → カレントから上
    /// （最後のは開発用。`export_model.py` の書き出しや `vendor/typo-normalizer` をそのまま使える）
    public static func defaultDirectoryURL() -> URL? {
        var candidates: [URL] = []
        if let path = ProcessInfo.processInfo.environment["IROHA_TYPO_MODEL"], !path.isEmpty {
            candidates.append(URL(fileURLWithPath: path, isDirectory: true))
        }
        candidates.append(TypoNormalizerInstall.directoryURL)
        var directory = URL(fileURLWithPath: FileManager.default.currentDirectoryPath, isDirectory: true)
        for _ in 0..<4 {
            candidates.append(directory.appendingPathComponent("vendor/typo-normalizer", isDirectory: true))
            candidates.append(directory.appendingPathComponent("export-scale-16x", isDirectory: true))
            directory.deleteLastPathComponent()
        }
        return candidates.first { isModelDirectory($0) }
    }

    public static func isModelDirectory(_ url: URL) -> Bool {
        FileManager.default.fileExists(atPath: url.appendingPathComponent("manifest.json").path)
            && FileManager.default.fileExists(atPath: url.appendingPathComponent("weights.bin").path)
    }

    /// モデルを読み込む（呼ばなくても最初の推論で読まれる）。失敗は一度だけ記録して以後は同じ結果を返す
    public func prewarm() throws {
        _ = try load()
    }

    private func load() throws -> Runtime {
        if let runtime { return runtime }
        if let loadFailure { throw loadFailure }
        do {
            let created = try Runtime(directory: directory)
            runtime = created
            return created
        } catch {
            loadFailure = error
            throw error
        }
    }

    /// 訂正案（しきい値を超えたときだけ）。読みが対象外・訂正なし・自信が足りないときは nil
    public func correction(
        for reading: String, threshold: Double = TypoNormalizer.defaultThreshold
    ) throws -> TypoCorrection? {
        let runtime = try load()
        guard runtime.supports(reading: reading) else { return nil }
        guard let corrected = runtime.generate(reading), corrected != reading else { return nil }
        // どちらも同じ入力に対する完全な系列なので直接比べられる（SWIFT-PORT.md §4）
        let margin = runtime.logProbability(of: corrected, given: reading)
            - runtime.logProbability(of: reading, given: reading)
        guard margin > threshold else { return nil }
        return TypoCorrection(reading: reading, corrected: corrected, margin: margin)
    }

    /// しきい値を通さない生の生成結果（検証・実験用）
    public func generate(for reading: String) throws -> String? {
        try load().generate(reading)
    }

    /// logP(target | source)。log_softmax したあと正解 id の値を全ステップ足す（EOS も含む）
    public func logProbability(of target: String, given source: String) throws -> Double {
        try load().logProbability(of: target, given: source)
    }

    /// teacher forcing で `target` を食わせたときの先頭 `steps` ステップのロジット（parity 検証用）
    public func logits(source: String, target: String, steps: Int) throws -> [[Float]] {
        try load().logits(source: source, target: target, steps: steps)
    }

    public func supports(reading: String) throws -> Bool {
        try load().supports(reading: reading)
    }

    // MARK: - 実体

    /// 語彙とモデルをまとめた非 Sendable の実体。アクターの中だけで触る
    private final class Runtime {
        private let vocabulary: TypoVocabulary
        private let model: TypoNormalizerModel
        private let maxLength: Int

        init(directory: URL) throws {
            guard TypoNormalizer.isModelDirectory(directory) else {
                throw LoadError.notFound(directory)
            }
            let manifestData = try Data(contentsOf: directory.appendingPathComponent("manifest.json"))
            let manifest = try JSONDecoder().decode(TypoNormalizerManifest.self, from: manifestData)
            let weightData = try Data(
                contentsOf: directory.appendingPathComponent("weights.bin"), options: .mappedIfSafe)
            let weights = try TypoNormalizerWeights(manifest: manifest, data: weightData)
            vocabulary = TypoVocabulary(manifest: manifest)
            model = try TypoNormalizerModel(manifest: manifest, weights: weights)
            maxLength = manifest.config.maxLength
        }

        func supports(reading: String) -> Bool {
            !reading.isEmpty
                && reading.count <= TypoNormalizer.maxReadingLength
                && vocabulary.containsAll(reading)
        }

        /// greedy + KV キャッシュ。生成長は読みの長さ + 9 まで（学習時と同じ）
        func generate(_ reading: String) -> String? {
            let source = vocabulary.encode(reading, eos: true)
            guard source.count <= maxLength else { return nil }
            let memory = model.encode(source)
            let cache = model.makeCache(memory: memory)
            let maxNew = min(maxLength - 1, source.count + 8)
            var generated: [Int] = []
            var token = TypoVocabulary.bos
            for step in 0..<maxNew {
                let logits = model.decode([token], cache: cache, offset: step)
                token = TypoMath.argmax(logits[logits.rows - 1], count: logits.cols)
                if token == TypoVocabulary.eos { break }
                generated.append(token)
            }
            return vocabulary.decode(generated)
        }

        /// teacher forcing の前向き 1 回で出る（生成と違って逐次でない）
        func logProbability(of target: String, given source: String) -> Double {
            let sourceIDs = vocabulary.encode(source, eos: true)
            let targetIDs = [TypoVocabulary.bos] + vocabulary.encode(target, eos: true)
            guard sourceIDs.count <= maxLength, targetIDs.count - 1 <= maxLength else { return -.infinity }
            let memory = model.encode(sourceIDs)
            let cache = model.makeCache(memory: memory)
            let logits = model.decode(Array(targetIDs.dropLast()), cache: cache, offset: 0)
            var total = 0.0
            for step in 0..<logits.rows {
                total += TypoMath.logSoftmaxValue(
                    logits[step], count: logits.cols, at: targetIDs[step + 1])
            }
            return total
        }

        func logits(source: String, target: String, steps: Int) -> [[Float]] {
            let sourceIDs = vocabulary.encode(source, eos: true)
            let targetIDs = [TypoVocabulary.bos] + vocabulary.encode(target, eos: true)
            let memory = model.encode(sourceIDs)
            let cache = model.makeCache(memory: memory)
            let logits = model.decode(Array(targetIDs.dropLast()), cache: cache, offset: 0)
            return (0..<min(steps, logits.rows)).map { step in
                let row = logits[step]
                return (0..<logits.cols).map { row[$0] }
            }
        }
    }
}

/// 文字単位の語彙（`training/typo-normalizer/tokenizer.py`）。
/// 先頭 4 つは `<pad> <s> </s> <unk>`、以降は 1 文字ずつ
struct TypoVocabulary {
    static let pad = 0
    static let bos = 1
    static let eos = 2
    static let unknown = 3

    private let itos: [String]
    private let stoi: [Character: Int]

    init(manifest: TypoNormalizerManifest) {
        itos = manifest.vocab
        var table: [Character: Int] = [:]
        for (index, text) in manifest.vocab.enumerated() where index >= 4 {
            if let character = text.first, text.count == 1 { table[character] = index }
        }
        stoi = table
    }

    /// 語彙に無い文字を含まないか。含むなら、そもそもモデルを呼ばない（SWIFT-PORT.md §8）
    func containsAll(_ text: String) -> Bool {
        text.allSatisfy { stoi[$0] != nil }
    }

    func encode(_ text: String, bos: Bool = false, eos: Bool = false) -> [Int] {
        var ids = text.map { stoi[$0] ?? Self.unknown }
        if bos { ids.insert(Self.bos, at: 0) }
        if eos { ids.append(Self.eos) }
        return ids
    }

    func decode(_ ids: [Int]) -> String {
        var result = ""
        for id in ids {
            if id == Self.pad || id == Self.bos { continue }
            if id == Self.eos { break }
            result += id < itos.count ? itos[id] : "?"
        }
        return result
    }
}
