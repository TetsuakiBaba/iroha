import Foundation

/// 追加学習（LoRA）のハイパーパラメータ。
///
/// 個人の記録はいまのモデルが既に正解できるものが大半（実測で 96.9%）で、それらは損失にほとんど
/// 寄与せず学習を薄めるだけなので、訓練データは**モデルが間違えた記録（`TrainingScreener`）を
/// 重み付けして**作り、正解できる記録は忘却を抑えるアンカーとして少量だけ混ぜる
public struct TrainingConfig: Sendable, Codable, Equatable {
    public var rank = 8
    public var alpha: Float = 16
    public var learningRate: Float = 5e-5
    public var epochs = 3
    public var batchSize = 16
    /// LoRA を掛けるテンソル名の末尾（`blk.N.<名前>.weight`）。出力層・埋め込みは対象外
    public var targets = ["attn_qkv", "attn_output", "ffn_up", "ffn_down"]

    /// モデルが間違えた記録を訓練データに入れる回数（希少なので重くする）
    public var mistakeWeight = 8
    /// 重み付け後の行数に対して混ぜるアンカー（モデルが正解できる記録）の比率。
    /// 0 にすると間違えた記録だけで学習し、忘却しやすくなる
    public var anchorRatio = 2.0

    /// 間違えた記録のうち評価に回す割合と上限（時系列の末尾から取る）
    public var heldOutMistakeFraction = 0.2
    public var maxHeldOutMistakes = 25
    /// 正解できる記録のうち評価（壊れていないかの確認）に回す上限
    public var maxHeldOutCorrect = 40

    public init() {}

    /// 学習対象の数に応じた既定。
    ///
    /// 既定は「効果より悪化を出さない」側に寄せてある（2026-09-18 実測: 記録 376 件・間違い 10 件では
    /// どの設定でも間違いは直らず、強い設定ほど元から正しかった変換を壊した。
    /// lr 1e-4/5 エポックで 38/40、lr 5e-5/3 エポック + アンカー 2 倍で 39/40）。
    /// 学べる例が増えたら学習率を上げる
    public static func recommended(forMistakeCount count: Int) -> TrainingConfig {
        var config = TrainingConfig()
        if count >= 50 {
            config.learningRate = 1e-4
            config.anchorRatio = 1.0
        }
        return config
    }
}

/// 学習 1 例（トークン化済み）
public struct TrainingExample: Sendable, Equatable {
    public let line: String
    /// `trainingLine` のトークン列 + 終端トークン
    public let tokens: [Int32]
    /// 損失を掛け始める位置。`targets[t] = tokens[t+1]` が U+EE01 の次のトークンになる t
    /// （= U+EE01 を表すトークン列の最後のインデックス）。`training/train.py` の `-100` マスクと同じ
    public let lossFrom: Int

    public init(line: String, tokens: [Int32], lossFrom: Int) {
        self.line = line
        self.tokens = tokens
        self.lossFrom = lossFrom
    }
}

/// 訓練用と評価用に振り分けた結果
public struct TrainingSplit: Sendable {
    /// 訓練に使う「モデルが間違えた記録」
    public var trainMistakes: [ConversionLogEntry] = []
    /// 訓練に混ぜるアンカー（モデルが正解できる記録）
    public var anchors: [ConversionLogEntry] = []
    /// 評価用の「間違えた記録」（学習で当たるようになったか。学習前は 0 件正解と分かっている）
    public var heldOutMistakes: [ConversionLogEntry] = []
    /// 評価用の「正解できる記録」（壊れていないか。学習前は全件正解と分かっている）
    public var heldOutCorrect: [ConversionLogEntry] = []
    /// 重み付け・混合済みの訓練行
    public var trainLines: [String] = []
}

public enum TrainingDataError: Error, CustomStringConvertible {
    case outputTagMissing(String)
    case noExamples

    public var description: String {
        switch self {
        case .outputTagMissing(let line): return "出力タグ(U+EE01)がトークン化されていません: \(line)"
        case .noExamples: return "学習に使える記録がありません"
        }
    }
}

/// `ConversionLog` の記録から学習データを作る。アーキテクチャ非依存（トークナイザは注入）
public enum TrainingDataBuilder {

    /// 学習に使える記録か。空の確定・かなを含まない読み・改行入りは除く
    public static func isUsable(_ entry: ConversionLogEntry) -> Bool {
        let committed = entry.committed.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !committed.isEmpty, !entry.reading.isEmpty else { return false }
        guard !entry.committed.contains(where: \.isNewline), !entry.reading.contains(where: \.isNewline),
              !entry.context.contains(where: \.isNewline) else { return false }
        let hasKana = entry.reading.unicodeScalars.contains {
            (0x3041...0x3096).contains($0.value) || (0x30A1...0x30FA).contains($0.value)
        }
        return hasKana
    }

    /// モデルの出力を直した確定か（`edited` が不明な F6〜F10 の確定も、ユーザが形を指定した以上は修正として扱う）
    public static func isCorrection(_ entry: ConversionLogEntry) -> Bool {
        entry.edited != false
    }

    /// 全ログファイルから使える記録を時系列順に集める。
    ///
    /// LoRA アダプタを適用している間に記録された確定（`entry.model` にアダプタ名が入る）も
    /// 分け隔てなく使う。記録は「ユーザがこれでよいと思って確定した結果」であり、
    /// どのモデルがその候補を出したかは学習価値と関係ないため（モデル名は由来の記録として残す）
    public static func collect(from log: ConversionLog) -> [ConversionLogEntry] {
        log.fileURLs()
            .flatMap { log.entries(in: $0) }
            .filter(isUsable)
            .sorted { $0.timestamp < $1.timestamp }
    }

    /// 同じ学習行（文脈・読み・確定が一致）は最後の 1 件だけ残す（順序は保つ）
    public static func dedupe(_ entries: [ConversionLogEntry]) -> [ConversionLogEntry] {
        var lastIndex: [String: Int] = [:]
        for (index, entry) in entries.enumerated() { lastIndex[entry.trainingLine] = index }
        return entries.enumerated().filter { lastIndex[$0.element.trainingLine] == $0.offset }.map(\.element)
    }

    /// 評価用を時系列の末尾から取り分けて訓練行を組み立てる（`TrainingScreener` の結果を渡す）
    public static func stratify(_ screening: TrainingScreener.Screening, config: TrainingConfig) -> TrainingSplit {
        let mistakes = screening.mistakes
        let correct = screening.correct

        // 評価用に取る「間違えた記録」。少ないときは 1 件でも取る（0 だと効果が測れない）。
        // ただし 3 分の 1 を超えて取らない（学習に残す分を確保する）。1 件しかなければ学習に回す
        let heldOutMistakeCount: Int = {
            guard mistakes.count >= 4 else { return mistakes.count >= 2 ? 1 : 0 }
            let target = max(2, Int((Double(mistakes.count) * config.heldOutMistakeFraction).rounded()))
            return min(target, config.maxHeldOutMistakes, mistakes.count / 3)
        }()
        let heldOutCorrectCount = min(correct.count / 2, config.maxHeldOutCorrect)

        var split = TrainingSplit()
        split.heldOutMistakes = Array(mistakes.suffix(heldOutMistakeCount))
        split.heldOutCorrect = Array(correct.suffix(heldOutCorrectCount))
        split.trainMistakes = Array(mistakes.dropLast(heldOutMistakeCount))

        let remainingCorrect = Array(correct.dropLast(heldOutCorrectCount))
        let weighted = split.trainMistakes.count * max(config.mistakeWeight, 1)
        let anchorCount = min(remainingCorrect.count, Int(Double(weighted) * max(config.anchorRatio, 0)))
        // アンカーは時系列全体から均等に取る（末尾に偏らせない）
        split.anchors = sample(remainingCorrect, count: anchorCount)

        split.trainLines =
            split.trainMistakes.flatMap { Array(repeating: $0.trainingLine, count: max(config.mistakeWeight, 1)) }
            + split.anchors.map(\.trainingLine)
        return split
    }

    /// `items` から `count` 件を均等間隔で取る（決定的。同じ入力なら同じ結果）
    static func sample<T>(_ items: [T], count: Int) -> [T] {
        guard count > 0, !items.isEmpty else { return [] }
        guard count < items.count else { return items }
        let step = Double(items.count) / Double(count)
        return (0..<count).map { items[min(items.count - 1, Int(Double($0) * step))] }
    }

    /// トークン化して損失位置を付ける。`outputTag` は U+EE01 のトークン列（`VocabTokenizer.outputTagTokens`。
    /// 1 トークンとは限らない）。行の中で最後に現れた位置を出力の始まりとみなす
    public static func encode(lines: [String], tokenize: (String) -> [Int32], eos: Int32, outputTag: [Int32]) throws
        -> [TrainingExample]
    {
        guard !outputTag.isEmpty else { throw TrainingDataError.outputTagMissing("(タグが空)") }
        var examples: [TrainingExample] = []
        for line in lines {
            var tokens = tokenize(line)
            guard let tagEnd = lastIndexOfSubsequence(outputTag, in: tokens) else {
                throw TrainingDataError.outputTagMissing(line)
            }
            tokens.append(eos)
            // タグの直後に終端しか無い（出力が空）例は学習に入れない
            guard tagEnd + 2 < tokens.count else { continue }
            examples.append(TrainingExample(line: line, tokens: tokens, lossFrom: tagEnd))
        }
        guard !examples.isEmpty else { throw TrainingDataError.noExamples }
        return examples
    }

    /// `pattern` が `tokens` の中で最後に現れる位置（末尾要素のインデックス）
    static func lastIndexOfSubsequence(_ pattern: [Int32], in tokens: [Int32]) -> Int? {
        guard pattern.count <= tokens.count else { return nil }
        var start = tokens.count - pattern.count
        while start >= 0 {
            if Array(tokens[start..<(start + pattern.count)]) == pattern { return start + pattern.count - 1 }
            start -= 1
        }
        return nil
    }

    /// held-out を `iroha-cli bench` が読める TSV（`読み\t正解\t文脈`）にする
    public static func heldOutTSV(_ entries: [ConversionLogEntry]) -> String {
        func clean(_ text: String) -> String {
            text.replacingOccurrences(of: "\t", with: " ").replacingOccurrences(of: "\n", with: " ")
        }
        return entries.map { "\(clean($0.reading))\t\(clean($0.committed))\t\(clean($0.context))" }
            .joined(separator: "\n") + (entries.isEmpty ? "" : "\n")
    }
}
