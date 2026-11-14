import Foundation

extension Benchmark {

    /// 精度の集計（acc@1 と MinCER）
    public struct Tally: Codable, Sendable, Equatable {
        public var count = 0
        public var hits = 0
        public var minCERSum = 0.0

        public init() {}

        public mutating func add(_ result: ItemResult) {
            count += 1
            if result.hit { hits += 1 }
            minCERSum += result.minCER
        }

        /// acc@1（0〜1）
        public var accuracy: Double { count > 0 ? Double(hits) / Double(count) : 0 }
        /// MinCER の平均（0〜1）
        public var meanMinCER: Double { count > 0 ? minCERSum / Double(count) : 0 }

        /// 「acc@1 172/200 (86.0%)  MinCER 2.10%」（`iroha-cli ajimee` の表示）
        public var summaryText: String {
            String(format: "acc@1 %d/%d (%.1f%%)  MinCER %.2f%%", hits, count, accuracy * 100, meanMinCER * 100)
        }
    }

    /// 変換時間の集計（ミリ秒）。`testdata/speed/summarize.py` の `stats` と同じ
    public struct Latency: Codable, Sendable, Equatable {
        public let count: Int
        public let mean: Double
        public let p50: Double
        public let p90: Double
        public let p99: Double
        public let max: Double

        public init(_ milliseconds: [Double]) {
            let sorted = milliseconds.sorted()
            count = sorted.count
            mean = sorted.isEmpty ? 0 : sorted.reduce(0, +) / Double(sorted.count)
            p50 = Latency.percentile(sorted, 50)
            p90 = Latency.percentile(sorted, 90)
            p99 = Latency.percentile(sorted, 99)
            max = sorted.last ?? 0
        }

        /// 最近順位法（nearest-rank）。p は 0〜100。`summarize.py` の `percentile` と同じ
        public static func percentile(_ sorted: [Double], _ p: Double) -> Double {
            guard !sorted.isEmpty else { return 0 }
            let rank = Swift.max(1, Int((p / 100 * Double(sorted.count)).rounded(.up)))
            return sorted[Swift.min(rank, sorted.count) - 1]
        }
    }

    /// 読みの文字数の区切り 1 つと、その中の中央値
    public struct LengthBin: Codable, Sendable, Equatable {
        public let lower: Int
        /// nil なら上限なし
        public let upper: Int?
        public let count: Int
        public let p50: Double

        public var label: String { upper.map { "\(lower)〜\($0)" } ?? "\(lower)〜" }
    }

    /// 読みの文字数の区切り（`summarize.py` の `LENGTH_BINS`）
    public static let lengthBins: [(Int, Int?)] = [(1, 10), (11, 20), (21, 30), (31, 40), (41, nil)]

    /// 1 回の計測の集計
    public struct Summary: Codable, Sendable, Equatable {
        public let total: Tally
        public let withContext: Tally
        public let withoutContext: Tally
        /// 出典ごと（出典の名前の順）。出典の無い評価セット（AJIMEE）では空
        public let perSource: [SourceTally]
        public let latency: Latency
        public let lengthBins: [LengthBin]
        /// 変換時間のうち NN の計算が占める割合（0〜1）。内訳が取れなかったら nil
        public let neuralNetworkShare: Double?

        public struct SourceTally: Codable, Sendable, Equatable {
            public let source: String
            public let tally: Tally
        }

        public init(_ results: [ItemResult]) {
            var total = Tally(), withContext = Tally(), withoutContext = Tally()
            var bySource: [String: Tally] = [:]
            for result in results {
                total.add(result)
                if result.contextLength > 0 { withContext.add(result) } else { withoutContext.add(result) }
                if let source = result.source { bySource[source, default: Tally()].add(result) }
            }
            self.total = total
            self.withContext = withContext
            self.withoutContext = withoutContext
            self.perSource = bySource.keys.sorted().map { SourceTally(source: $0, tally: bySource[$0]!) }
            self.latency = Latency(results.map(\.milliseconds))
            self.lengthBins = Benchmark.lengthBins.compactMap { lower, upper in
                let part = results.filter { $0.readingLength >= lower && (upper == nil || $0.readingLength <= upper!) }
                guard !part.isEmpty else { return nil }
                return LengthBin(lower: lower, upper: upper, count: part.count, p50: Latency(part.map(\.milliseconds)).p50)
            }
            let totalTime = results.reduce(0) { $0 + $1.milliseconds }
            let neuralNetwork = results.reduce(0) { $0 + $1.neuralNetworkMilliseconds }
            self.neuralNetworkShare = totalTime > 0 && neuralNetwork > 0 ? neuralNetwork / totalTime : nil
        }
    }
}
