import Foundation

/// 追加学習（LoRA）のハイパーパラメータ。
///
/// 訓練データは**記録そのまま**（評価用に取り分けた分を除く全件）。損失は確定文字列（U+EE01 の後ろ）に
/// しかかからず、条件はその人の左文脈と読みなので、記録 1 件は「この人の文脈ではこう書く」の 1 サンプルになる。
/// 間違えた記録だけを重み付けして学ぶような細工はしない（少ない記録で効果を出そうとすると偏った尾部に
/// 過剰適合し、できていた変換が崩れる。記録は増えていくものなので、増えた分だけ素直に効く設計にする）
public struct TrainingConfig: Sendable, Codable, Equatable {
    public var rank = 8
    public var alpha: Float = 16
    /// LoRA の一般的な既定値。設定画面から変えられる
    public var learningRate: Float = 1e-4
    public var epochs = 3
    public var batchSize = 16
    /// LoRA を掛けるテンソル名の末尾（`blk.N.<名前>.weight`）。出力層・埋め込みは対象外
    public var targets = ["attn_qkv", "attn_output", "ffn_up", "ffn_down"]

    public init() {}
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
    /// 評価用の「間違えた記録」（学習で当たるようになったか）
    public var heldOutMistakes: [ConversionLogEntry] = []
    /// 評価用の「正解できる記録」（壊れていないか）
    public var heldOutCorrect: [ConversionLogEntry] = []
    /// 訓練に使う記録（評価用を除いた全件・時系列順）
    public var train: [ConversionLogEntry] = []
    /// `train` の学習行
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

    /// 評価用に取り分ける上限。間違いは希少なので、効果を測るために新しい方から集める
    public static let maxHeldOutMistakes = 25
    /// 正解できていた記録の評価用（壊れていないかの確認）の上限
    public static let maxHeldOutCorrect = 40

    /// 評価用を時系列の末尾から取り分け、残り全件を訓練行にする。
    ///
    /// - Parameters:
    ///   - entries: 学習に使える記録の全件（重複除去済み・時系列順）。変換し直しの上限（`TrainingScreener`）に
    ///     入らなかった古い記録も訓練には使う
    ///   - screening: `entries`（の末尾）を変換し直した結果
    public static func stratify(entries: [ConversionLogEntry], screening: TrainingScreener.Screening) -> TrainingSplit {
        let mistakes = screening.mistakes
        let correct = screening.correct

        // 間違いは 3 分の 1 まで評価に回す（学習に残す分を確保する）。2 件以上あれば 1 件は取る
        // （0 だと効果が測れない）。1 件しかなければ学習に回す（唯一の例を評価に取られると学ぶものが無くなる）
        let heldOutMistakeCount = min(Self.maxHeldOutMistakes, max(mistakes.count >= 2 ? 1 : 0, mistakes.count / 3))
        let heldOutCorrectCount = min(Self.maxHeldOutCorrect, correct.count / 2)

        var split = TrainingSplit()
        split.heldOutMistakes = Array(mistakes.suffix(heldOutMistakeCount))
        split.heldOutCorrect = Array(correct.suffix(heldOutCorrectCount))
        // 記録は学習行（文脈・読み・確定）で重複除去済みなので、学習行で同一性を見てよい
        let heldOut = Set((split.heldOutMistakes + split.heldOutCorrect).map(\.trainingLine))
        split.train = entries.filter { !heldOut.contains($0.trainingLine) }
        split.trainLines = split.train.map(\.trainingLine)
        return split
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
