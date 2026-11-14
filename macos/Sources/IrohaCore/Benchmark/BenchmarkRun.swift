import Foundation

/// アプリ内ベンチマークの 1 回の計測の記録と、`iroha-bench` が標準出力に書く進み具合。
///
/// 記録は `<データフォルダ>/logs/bench/<日時>-<ホスト名>.json`（要約）と同じ名前の `.tsv`
/// （項目ごとの時間。`IROHA_TIMINGS` と同じ列なので `testdata/speed/summarize.py` で読める）に置く
public enum BenchmarkRun {

    /// 辞書ラティスの使い方（`iroha-cli` の `IROHA_LATTICE` と同じ 2 条件。`training/README.md` の「比較の標準条件」）
    public enum Lattice: String, Codable, Sendable, CaseIterable {
        /// モデルだけ（ライブ変換の第一候補と同じ経路）
        case off
        /// 辞書ラティスの候補をモデルで採点し直し、第一候補もそこから選ぶ
        case always

        public var displayName: String {
            switch self {
            case .off: return "モデルだけ"
            case .always: return "辞書ラティス + 再採点"
            }
        }
    }

    /// 計測の条件
    public struct Condition: Codable, Sendable, Equatable {
        public var lattice: Lattice
        /// 推論エンジン（"llama" / "mlx"）
        public var backend: String
        /// モデルのファイル名
        public var model: String
        /// LoRA アダプタのファイル名（無ければ nil）
        public var adapter: String?
        /// 推論の実行先。"CPU" か「ggml の装置名: 説明」（"MTL0: Apple M2"・"Vulkan0: NVIDIA GeForce RTX 4060" など）。
        /// MLX なら「MLX: <チップ名>」。形式 1 の記録には無い
        public var device: String?
        /// llama.cpp が生成に使う CPU のスレッド数。MLX と形式 1 の記録では nil
        public var threads: Int?

        public init(lattice: Lattice, backend: String, model: String, adapter: String?,
                    device: String? = nil, threads: Int? = nil) {
            self.lattice = lattice
            self.backend = backend
            self.model = model
            self.adapter = adapter
            self.device = device
            self.threads = threads
        }

        /// 「辞書ラティス + 再採点、llama.cpp（MTL0: Apple M2、4 スレッド）、モデル …」の形の 1 行
        public var summaryText: String {
            var engine = backend == "mlx" ? "MLX" : "llama.cpp"
            let detail = [device, threads.map { "\($0) スレッド" }].compactMap { $0 }
            if !detail.isEmpty { engine += "（" + detail.joined(separator: "、") + "）" }
            return "\(lattice.displayName)、\(engine)、モデル \(model)" + (adapter.map { "＋アダプタ \($0)" } ?? "")
        }
    }

    /// 速度を左右する、計測した機械の状態。macOS版・Windows版・Linux版で同じ形にする
    /// （`testdata/speed/README.md` の「アプリ内のベンチマークの記録」に OS ごとの取り方がある）。
    /// その OS で取れない項目は nil にする（推し量った値を入れない）
    public struct Environment: Codable, Sendable, Equatable {
        /// "macos" / "windows" / "linux"
        public var platform: String
        public var appVersion: String
        public var hostName: String
        /// 機種（macOS は sysctl hw.model の "Mac14,2" など）。分からなければ nil
        public var machineModel: String?
        /// CPU の名前（"Apple M2" など）
        public var cpu: String
        /// 論理コアの数
        public var cpuCores: Int?
        public var osVersion: String
        /// "ac" / "battery"。分からなければ nil
        public var powerSource: String?
        /// 省電力の設定が効いていたか（macOS の低電力モード・Windows の省電力モード・Linux の power-saver）。分からなければ nil
        public var powerSaving: Bool?
        /// 計測の前後の発熱の状態（"nominal" / "fair" / "serious" / "critical"。macOS の `ProcessInfo.thermalState`）。
        /// 取れない OS では nil
        public var thermalStateBefore: String?
        public var thermalStateAfter: String?

        public init(platform: String, appVersion: String, hostName: String, machineModel: String?, cpu: String,
                    cpuCores: Int?, osVersion: String, powerSource: String?, powerSaving: Bool?,
                    thermalStateBefore: String?, thermalStateAfter: String?) {
            self.platform = platform
            self.appVersion = appVersion
            self.hostName = hostName
            self.machineModel = machineModel
            self.cpu = cpu
            self.cpuCores = cpuCores
            self.osVersion = osVersion
            self.powerSource = powerSource
            self.powerSaving = powerSaving
            self.thermalStateBefore = thermalStateBefore
            self.thermalStateAfter = thermalStateAfter
        }

        private enum CodingKeys: String, CodingKey {
            case platform, appVersion, hostName, machineModel, cpu, cpuCores, osVersion, powerSource, powerSaving,
                 thermalStateBefore, thermalStateAfter
            /// 形式 1（2026-11-14 の最初の版。macOS だけ）の名前
            case chip, lowPowerMode
        }

        public init(from decoder: Decoder) throws {
            let c = try decoder.container(keyedBy: CodingKeys.self)
            platform = try c.decodeIfPresent(String.self, forKey: .platform) ?? "macos"
            appVersion = try c.decode(String.self, forKey: .appVersion)
            hostName = try c.decode(String.self, forKey: .hostName)
            machineModel = try c.decodeIfPresent(String.self, forKey: .machineModel)
            cpu = try c.decodeIfPresent(String.self, forKey: .cpu) ?? c.decode(String.self, forKey: .chip)
            cpuCores = try c.decodeIfPresent(Int.self, forKey: .cpuCores)
            osVersion = try c.decode(String.self, forKey: .osVersion)
            powerSource = try c.decodeIfPresent(String.self, forKey: .powerSource)
            powerSaving = try c.decodeIfPresent(Bool.self, forKey: .powerSaving)
                ?? c.decodeIfPresent(Bool.self, forKey: .lowPowerMode)
            thermalStateBefore = try c.decodeIfPresent(String.self, forKey: .thermalStateBefore)
            thermalStateAfter = try c.decodeIfPresent(String.self, forKey: .thermalStateAfter)
        }

        public func encode(to encoder: Encoder) throws {
            var c = encoder.container(keyedBy: CodingKeys.self)
            try c.encode(platform, forKey: .platform)
            try c.encode(appVersion, forKey: .appVersion)
            try c.encode(hostName, forKey: .hostName)
            try c.encodeIfPresent(machineModel, forKey: .machineModel)
            try c.encode(cpu, forKey: .cpu)
            try c.encodeIfPresent(cpuCores, forKey: .cpuCores)
            try c.encode(osVersion, forKey: .osVersion)
            try c.encodeIfPresent(powerSource, forKey: .powerSource)
            try c.encodeIfPresent(powerSaving, forKey: .powerSaving)
            try c.encodeIfPresent(thermalStateBefore, forKey: .thermalStateBefore)
            try c.encodeIfPresent(thermalStateAfter, forKey: .thermalStateAfter)
        }

        /// 速度の数字が落ちている恐れがある状態（バッテリー・省電力・発熱）の説明。無ければ nil
        public var throttlingNote: String? {
            var notes: [String] = []
            if powerSource == "battery" { notes.append("電源 バッテリー") }
            if powerSaving == true { notes.append("省電力の設定") }
            if let before = thermalStateBefore, let after = thermalStateAfter,
               before != "nominal" || after != "nominal" {
                notes.append("発熱の状態 \(before) → \(after)")
            }
            return notes.isEmpty ? nil : notes.joined(separator: "・")
        }

        public static func thermalStateName(_ state: ProcessInfo.ThermalState) -> String {
            switch state {
            case .nominal: return "nominal"
            case .fair: return "fair"
            case .serious: return "serious"
            case .critical: return "critical"
            @unknown default: return "unknown"
            }
        }
    }

    /// 正解しなかった項目（設定画面で見返す）
    public struct Miss: Codable, Sendable, Equatable {
        public let index: String
        public let context: String
        public let input: String
        public let output: String
        public let expected: [String]
    }

    /// 1 回の計測の記録（要約のファイル）
    public struct Record: Codable, Sendable, Equatable, Identifiable {
        /// 記録の形式。2 で OS 共通の形にした（2026-11-14）。キーが無い記録は形式 1（macOS だけの形）
        public static let currentFormatVersion = 2

        public let formatVersion: Int?
        /// ファイル名（拡張子なし）
        public let id: String
        public let benchmarkID: String
        public let benchmarkName: String
        public let condition: Condition
        public let environment: Environment
        public let startedAt: Date
        public let finishedAt: Date
        /// モデルの読み込みを含む、最初の 1 回の変換の時間（ミリ秒）
        public let warmUpMilliseconds: Double
        /// 計測中にキーボードの入力があったか（あれば速度の数字は IME の変換と取り合った値）。
        /// 確かめられない環境（Linux の Wayland など）では nil
        public let inputDuringRun: Bool?
        public let summary: Benchmark.Summary
        public let misses: [Miss]

        public init(id: String, benchmarkID: String, benchmarkName: String, condition: Condition,
                    environment: Environment, startedAt: Date, finishedAt: Date, warmUpMilliseconds: Double,
                    inputDuringRun: Bool?, summary: Benchmark.Summary, misses: [Miss]) {
            self.formatVersion = Record.currentFormatVersion
            self.id = id
            self.benchmarkID = benchmarkID
            self.benchmarkName = benchmarkName
            self.condition = condition
            self.environment = environment
            self.startedAt = startedAt
            self.finishedAt = finishedAt
            self.warmUpMilliseconds = warmUpMilliseconds
            self.inputDuringRun = inputDuringRun
            self.summary = summary
            self.misses = misses
        }

        public static func misses(items: [Benchmark.Item], results: [Benchmark.ItemResult]) -> [Miss] {
            zip(items, results).compactMap { item, result in
                result.hit ? nil : Miss(index: item.index, context: item.contextText, input: item.input,
                                        output: result.output, expected: item.expectedOutput)
            }
        }
    }

    // MARK: - 置き場所

    public static var directoryURL: URL {
        DataDirectory.logsURL.appendingPathComponent("bench", isDirectory: true)
    }

    /// 記録の名前（「20261114-153012-<ホスト名>」）
    public static func makeID(date: Date, hostName: String) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        let host = hostName.unicodeScalars.map {
            CharacterSet.alphanumerics.contains($0) || $0 == "-" ? String($0) : "_"
        }.joined()
        return "\(formatter.string(from: date))-\(host)"
    }

    public static func recordURL(id: String) -> URL { directoryURL.appendingPathComponent("\(id).json") }
    public static func timingsURL(id: String) -> URL { directoryURL.appendingPathComponent("\(id).tsv") }

    public static func write(_ record: Record, results: [Benchmark.ItemResult]) throws {
        try FileManager.default.createDirectory(at: directoryURL, withIntermediateDirectories: true)
        try encoder.encode(record).write(to: recordURL(id: record.id), options: .atomic)
        try Data(Benchmark.timingsTSV(results).utf8).write(to: timingsURL(id: record.id), options: .atomic)
    }

    /// 残っている記録。新しい順。データフォルダを共有している別の Mac の記録も含む
    public static func records() -> [Record] {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: directoryURL.path) else { return [] }
        return names.filter { $0.hasSuffix(".json") }
            .compactMap { try? decoder.decode(Record.self, from: Data(contentsOf: directoryURL.appendingPathComponent($0))) }
            .sorted { $0.startedAt > $1.startedAt }
    }

    public static func delete(id: String) {
        try? FileManager.default.removeItem(at: recordURL(id: id))
        try? FileManager.default.removeItem(at: timingsURL(id: id))
    }

    static var encoder: JSONEncoder {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        return encoder
    }

    static var decoder: JSONDecoder {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return decoder
    }

    // MARK: - iroha-bench の進み具合（標準出力の JSON Lines、1 行 1 件）

    public struct Event: Codable, Sendable, Equatable {
        public enum Kind: String, Codable, Sendable {
            /// モデルの読み込み中（最初の 1 回の変換）
            case loading
            /// 項目を 1 つ測り終えた
            case progress
            /// 計測を終えて記録を書いた
            case finished
            case failed
        }

        public let kind: Kind
        public var done: Int?
        public var total: Int?
        public var hits: Int?
        /// finished のときの記録の ID
        public var recordID: String?
        public var message: String?

        public init(kind: Kind, done: Int? = nil, total: Int? = nil, hits: Int? = nil,
                    recordID: String? = nil, message: String? = nil) {
            self.kind = kind
            self.done = done
            self.total = total
            self.hits = hits
            self.recordID = recordID
            self.message = message
        }

        public func jsonLine() -> String {
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
            let data = (try? encoder.encode(self)) ?? Data()
            return String(decoding: data, as: UTF8.self) + "\n"
        }

        public static func parse(_ line: String) -> Event? {
            try? JSONDecoder().decode(Event.self, from: Data(line.utf8))
        }
    }

    // MARK: - 結果の Markdown（設定画面の「Markdown でコピー」）

    public static func markdown(_ record: Record) -> String {
        let s = record.summary
        let e = record.environment
        var lines: [String] = []
        let date = DateFormatter.localizedString(from: record.startedAt, dateStyle: .short, timeStyle: .short)
        lines.append("## iroha ベンチマーク（\(record.benchmarkName)、\(date)）")
        lines.append("")
        lines.append("- 条件: \(record.condition.summaryText)")
        lines.append("- 機械: \(e.cpu)" + (e.machineModel.map { "（\($0)）" } ?? "")
            + "、\(e.osVersion)、iroha \(e.appVersion)、電源 "
            + (e.powerSource == "battery" ? "バッテリー" : e.powerSource == "ac" ? "AC" : "不明")
            + (e.powerSaving == true ? "（省電力の設定）" : "")
            + ((e.thermalStateBefore).map { "、発熱の状態 \($0) → \(e.thermalStateAfter ?? "?")" } ?? ""))
        switch record.inputDuringRun {
        case true?: lines.append("- 計測中にキーボードの入力があった")
        case nil: lines.append("- 計測中のキーボードの入力は確かめられない")
        case false?: break
        }
        lines.append("")
        lines.append("| 区分 | 件数 | acc@1 | MinCER |")
        lines.append("|---|---:|---:|---:|")
        func row(_ label: String, _ t: Benchmark.Tally) {
            guard t.count > 0 else { return }
            lines.append(String(format: "| %@ | %d | %d (%.1f%%) | %.2f%% |", label, t.count, t.hits,
                                t.accuracy * 100, t.meanMinCER * 100))
        }
        row("全体", s.total)
        row("文脈あり", s.withContext)
        row("文脈なし", s.withoutContext)
        for source in s.perSource { row(source.source, source.tally) }
        lines.append("")
        lines.append("| 件数 | 平均 | 中央値 | 90% | 99% | 最大 | NN の割合 |")
        lines.append("|---:|---:|---:|---:|---:|---:|---:|")
        let l = s.latency
        let share = s.neuralNetworkShare.map { String(format: "%.0f%%", $0 * 100) } ?? "—"
        lines.append(String(format: "| %d | %.1f | %.1f | %.1f | %.1f | %.1f | %@ |",
                            l.count, l.mean, l.p50, l.p90, l.p99, l.max, share))
        lines.append("")
        lines.append("（単位はミリ秒。1 件 = 読み 1 つの変換 1 回。モデルの読み込みを含む最初の変換 "
            + String(format: "%.0f", record.warmUpMilliseconds) + " ms は除く）")
        lines.append("")
        lines.append("読みの文字数別の中央値（括弧内は件数）")
        lines.append("")
        lines.append("| " + s.lengthBins.map(\.label).joined(separator: " | ") + " |")
        lines.append("|" + String(repeating: "---:|", count: s.lengthBins.count))
        lines.append("| " + s.lengthBins.map { String(format: "%.1f (%d)", $0.p50, $0.count) }.joined(separator: " | ") + " |")
        return lines.joined(separator: "\n") + "\n"
    }
}
