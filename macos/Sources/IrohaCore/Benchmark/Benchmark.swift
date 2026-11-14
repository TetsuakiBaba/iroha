import Foundation

/// 変換の精度と速度のベンチマーク（評価セットを 1 件ずつ変換して、正解との一致と時間を測る）。
///
/// `iroha-cli ajimee` と、アプリから起動する `iroha-bench`（設定 > モデル > ベンチマーク）が同じ実装を使う。
/// 採点は AJIMEE-Bench（azooKey/AJIMEE-Bench）と同じ: 第一候補を許容解のリストと照合し、acc@1（どれかと完全に一致）と
/// MinCER（許容解ごとの CER = 編集距離 / 正解の長さ、の最小値を項目ごとに取って平均）を出す。
/// 速度の集計は `testdata/speed/summarize.py` と同じ定義（最近順位法のパーセンタイル・読みの文字数の区切り）
public enum Benchmark {

    /// 評価セットの 1 項目（AJIMEE-Bench と iroha-bench-v1 の共通の形）
    public struct Item: Codable, Sendable, Equatable {
        public let index: String
        /// 出典（iroha-bench-v1 の `source`。AJIMEE には無い）
        public let source: String?
        public let contextText: String
        /// 読み（カタカナ）
        public let input: String
        public let expectedOutput: [String]

        public init(index: String, source: String? = nil, contextText: String, input: String, expectedOutput: [String]) {
            self.index = index
            self.source = source
            self.contextText = contextText
            self.input = input
            self.expectedOutput = expectedOutput
        }

        enum CodingKeys: String, CodingKey {
            case index, source, input
            case contextText = "context_text"
            case expectedOutput = "expected_output"
        }
    }

    public enum LoadError: LocalizedError, Equatable {
        case empty
        case invalidLine(Int)

        public var errorDescription: String? {
            switch self {
            case .empty: return "評価セットに項目がありません"
            case .invalidLine(let line): return "評価セットの \(line) 行目が読めません"
            }
        }
    }

    /// 評価セットを読む。JSON の配列（`evaluation_items.json`）と JSON Lines（配布用の `.jsonl`）のどちらも受け付ける
    public static func loadItems(from data: Data) throws -> [Item] {
        let decoder = JSONDecoder()
        let firstByte = data.first { !($0 == 0x20 || $0 == 0x0A || $0 == 0x0D || $0 == 0x09) }
        let items: [Item]
        if firstByte == UInt8(ascii: "[") {
            items = try decoder.decode([Item].self, from: data)
        } else {
            var parsed: [Item] = []
            var lineNumber = 0
            for line in data.split(separator: 0x0A, omittingEmptySubsequences: false) {
                lineNumber += 1
                let trimmed = line.drop { $0 == 0x20 || $0 == 0x0D || $0 == 0x09 }
                if trimmed.isEmpty { continue }
                guard let item = try? decoder.decode(Item.self, from: Data(line)) else {
                    throw LoadError.invalidLine(lineNumber)
                }
                parsed.append(item)
            }
            items = parsed
        }
        guard !items.isEmpty else { throw LoadError.empty }
        return items
    }

    public static func loadItems(contentsOf url: URL) throws -> [Item] {
        try loadItems(from: Data(contentsOf: url))
    }

    // MARK: - 採点

    /// 1 項目の採点
    public struct Score: Sendable, Equatable {
        public let hit: Bool
        public let minCER: Double
    }

    public static func score(output: String, expected: [String]) -> Score {
        let hit = expected.contains(output)
        let candidate = Array(output)
        let minCER = expected.map { reference in
            Double(editDistance(candidate, Array(reference))) / Double(max(reference.count, 1))
        }.min() ?? 1.0
        return Score(hit: hit, minCER: minCER)
    }

    /// 文字単位の編集距離（レーベンシュタイン距離）
    public static func editDistance(_ a: [Character], _ b: [Character]) -> Int {
        if a.isEmpty { return b.count }
        if b.isEmpty { return a.count }
        var previous = Array(0...b.count)
        var current = [Int](repeating: 0, count: b.count + 1)
        for i in 1...a.count {
            current[0] = i
            for j in 1...b.count {
                let cost = a[i - 1] == b[j - 1] ? 0 : 1
                current[j] = min(previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost)
            }
            swap(&previous, &current)
        }
        return previous[b.count]
    }

    // MARK: - 1 項目の結果

    /// 1 項目を変換した結果。時間はミリ秒
    public struct ItemResult: Codable, Sendable, Equatable {
        public let index: String
        public let source: String?
        public let readingLength: Int
        public let contextLength: Int
        public let output: String
        public let hit: Bool
        public let minCER: Double
        /// 変換 1 回の時間（エンジンの呼び出しの前後）
        public let milliseconds: Double
        /// NN の時間の内訳（`InferenceTimer.Snapshot`）。llama.cpp の推論でだけ入る
        public let promptMilliseconds: Double
        public let decodeMilliseconds: Double
        public let decodeSteps: Int
        public let selectMilliseconds: Double

        public init(item: Item, output: String, milliseconds: Double, timing: InferenceTimer.Snapshot?) {
            let score = Benchmark.score(output: output, expected: item.expectedOutput)
            self.index = item.index
            self.source = item.source
            self.readingLength = item.input.count
            self.contextLength = item.contextText.count
            self.output = output
            self.hit = score.hit
            self.minCER = score.minCER
            self.milliseconds = milliseconds
            self.promptMilliseconds = timing.map { Benchmark.milliseconds($0.prompt) } ?? 0
            self.decodeMilliseconds = timing.map { Benchmark.milliseconds($0.decode) } ?? 0
            self.decodeSteps = timing?.decodeSteps ?? 0
            self.selectMilliseconds = timing.map { Benchmark.milliseconds($0.tokenSelection) } ?? 0
        }

        /// NN の計算の合計（内訳の和。`summarize.py` の NN の割合と同じ）
        public var neuralNetworkMilliseconds: Double {
            promptMilliseconds + decodeMilliseconds + selectMilliseconds
        }
    }

    public static func milliseconds(_ duration: Duration) -> Double {
        Double(duration.components.attoseconds) / 1e15 + Double(duration.components.seconds) * 1e3
    }

    /// 1 項目を変換して測る。変換に失敗したら読みをそのまま答えとして扱う（`iroha-cli ajimee` と同じ）
    public static func run(_ item: Item, engine: any ConversionEngine) async -> ItemResult {
        let timer = InferenceTimer()
        let start = ContinuousClock.now
        let output = await InferenceTimer.$current.withValue(timer) {
            (try? await engine.convert(reading: item.input, context: item.contextText, candidateCount: 1).first)
                ?? item.input
        }
        let elapsed = milliseconds(start.duration(to: .now))
        return ItemResult(item: item, output: output, milliseconds: elapsed, timing: timer.snapshot)
    }

    /// 計測の前に 1 回変換して、モデルの読み込みを計測から外す。読み込みにかかった時間（ミリ秒）を返す
    @discardableResult
    public static func warmUp(engine: any ConversionEngine) async -> Double {
        let start = ContinuousClock.now
        _ = try? await engine.convert(reading: "うぉーむあっぷ", context: "", candidateCount: 1)
        return milliseconds(start.duration(to: .now))
    }

    // MARK: - 時間の TSV

    /// `IROHA_TIMINGS` と同じ列（`testdata/speed/summarize.py` で読める）
    public static let timingsHeader = "index\treading_length\tcontext_length\tms\tprompt_ms\tdecode_ms\tdecode_steps\tselect_ms\n"

    public static func timingsLine(_ result: ItemResult) -> String {
        "\(result.index)\t\(result.readingLength)\t\(result.contextLength)\t"
            + String(format: "%.3f\t%.3f\t%.3f\t%d\t%.3f", result.milliseconds, result.promptMilliseconds,
                     result.decodeMilliseconds, result.decodeSteps, result.selectMilliseconds) + "\n"
    }

    public static func timingsTSV(_ results: [ItemResult]) -> String {
        timingsHeader + results.map(timingsLine).joined()
    }
}
