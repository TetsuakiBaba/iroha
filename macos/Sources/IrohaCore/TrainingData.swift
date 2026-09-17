import Foundation

/// 追加学習（LoRA）のハイパーパラメータ。少量の個人データで壊滅的忘却を起こさない側に寄せた既定値
public struct TrainingConfig: Sendable, Codable, Equatable {
    public var rank = 8
    public var alpha: Float = 16
    public var learningRate: Float = 1e-4
    public var epochs = 3
    public var batchSize = 16
    /// LoRA を掛けるテンソル名の末尾（`blk.N.<名前>.weight`）。出力層・埋め込みは対象外
    public var targets = ["attn_qkv", "attn_output", "ffn_up", "ffn_down"]
    /// 時系列の末尾から評価用に取り分ける割合と最低件数
    public var heldOutFraction = 0.1
    public var minHeldOut = 20
    /// 修正して確定した行（`edited == true`）を 2 回入れる（少データで「直した変換」を強調する）
    public var duplicateEdited = true

    public init() {}

    /// データ量に応じた既定（200 行未満なら epochs を増やす）
    public static func recommended(forExampleCount count: Int) -> TrainingConfig {
        var config = TrainingConfig()
        if count < 200 { config.epochs = 5 }
        return config
    }
}

/// 学習 1 例（トークン化済み）
public struct TrainingExample: Sendable, Equatable {
    public let line: String
    /// `trainingLine` のトークン列 + EOS
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

    /// 全ログファイルから使える記録を時系列順に集める
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

    /// 時系列の末尾を評価用（held-out）に取り分ける。件数は `fraction` と `minimum` の大きい方、
    /// ただし全体の半分まで
    public static func split(_ entries: [ConversionLogEntry], heldOutFraction fraction: Double, minHeldOut minimum: Int)
        -> (train: [ConversionLogEntry], heldOut: [ConversionLogEntry])
    {
        let count = min(max(Int(Double(entries.count) * fraction), minimum), entries.count / 2)
        guard count > 0 else { return (entries, []) }
        return (Array(entries.dropLast(count)), Array(entries.suffix(count)))
    }

    /// 学習行にする。`duplicateEdited` なら修正した確定を 2 回入れる
    public static func trainingLines(_ entries: [ConversionLogEntry], duplicateEdited: Bool) -> [String] {
        entries.flatMap { entry -> [String] in
            let line = entry.trainingLine
            return duplicateEdited && entry.edited == true ? [line, line] : [line]
        }
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
            // タグの直後に EOS しか無い（出力が空）例は学習に入れない
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
