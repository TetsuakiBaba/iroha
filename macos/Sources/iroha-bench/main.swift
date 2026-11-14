import CoreGraphics
import Foundation
import IOKit.ps
import CLlama
import IrohaCore
import IrohaEngine
import IrohaMLX

// 変換の精度と速度のベンチマークを測るヘルパー。IME 本体（設定 > モデル > ベンチマーク）が Process で起動し、
// 標準出力の JSON Lines（BenchmarkRun.Event）で進み具合を受け取る。記録は <データフォルダ>/logs/bench/ に書く。
//
// 使い方:
//   iroha-bench run --set <評価セット.jsonl|.json> --model <model.gguf>
//                   [--id <評価セットのID>] [--name <表示名>] [--adapter <adapter.gguf>]
//                   [--backend llama|mlx] [--lattice off|always] [--app-version <版>] [--limit N]
//   環境変数 IROHA_DATA_DIR でデータフォルダ（記録の置き場所）を上書き。無ければ IME 本体の設定に従う
//
// ユーザ辞書・学習・自分の原稿の表記・専門辞書は使わない（iroha-cli ajimee と同じく、ほかの Mac の数字と
// 比べられるようにするため）。エンジンの重ね方は IME と同じ（EngineAssembly）

setvbuf(stdout, nil, _IOLBF, 0)  // 行ごとに流す（親プロセスが逐次読む）

// llama.cpp のログは stderr へ（stdout は JSON 専用）
llama_log_set({ level, text, _ in
    guard let text, level.rawValue >= GGML_LOG_LEVEL_WARN.rawValue else { return }
    FileHandle.standardError.write(Data(String(cString: text).utf8))
}, nil)

func emit(_ event: BenchmarkRun.Event) {
    FileHandle.standardOutput.write(Data(event.jsonLine().utf8))
}

/// 終える。`exit` だと静的な後始末で llama.cpp の Metal の解放が走り、モデルを持ったままなので
/// GGML_ASSERT で異常終了する（2026-11-14 実測。記録は書けているがクラッシュレポートが残る）。後始末は飛ばす
func finish(_ status: Int32) -> Never {
    _exit(status)
}

func fail(_ message: String) -> Never {
    emit(BenchmarkRun.Event(kind: .failed, message: message))
    finish(1)
}

func usage() -> Never {
    FileHandle.standardError.write(Data("""
    使い方:
      iroha-bench run --set <評価セット.jsonl|.json> --model <model.gguf>
                      [--id <ID>] [--name <表示名>] [--adapter <adapter.gguf>]
                      [--backend llama|mlx] [--lattice off|always] [--app-version <版>] [--limit N]

    """.utf8))
    exit(2)
}

func configureDataDirectory() {
    let env = ProcessInfo.processInfo.environment
    if let path = env["IROHA_DATA_DIR"], !path.isEmpty {
        DataDirectory.configure(URL(fileURLWithPath: path, isDirectory: true))
    } else if let path = UserDefaults(suiteName: "dev.iroha.inputmethod.iroha")?
        .string(forKey: "dataDirectory"), !path.isEmpty {
        DataDirectory.configure(URL(fileURLWithPath: path, isDirectory: true))
    }
}

// MARK: - 機械の状態

func sysctlString(_ name: String) -> String? {
    var size = 0
    guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return nil }
    var buffer = [CChar](repeating: 0, count: size)
    guard sysctlbyname(name, &buffer, &size, nil, 0) == 0 else { return nil }
    return String(cString: buffer)
}

/// "ac" / "battery"（分からなければ nil）
func powerSource() -> String? {
    guard let info = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
          let type = IOPSGetProvidingPowerSourceType(info)?.takeUnretainedValue() as String? else { return nil }
    if type == kIOPMACPowerKey { return "ac" }
    if type == kIOPMBatteryPowerKey { return "battery" }
    return nil
}

func appVersion(_ given: String?) -> String {
    if let given, !given.isEmpty { return given }
    return Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "?"
}

// MARK: - 引数

var options: [String: String] = [:]
let arguments = CommandLine.arguments
guard arguments.count >= 2, arguments[1] == "run" else { usage() }
var index = 2
while index < arguments.count {
    let key = arguments[index]
    guard key.hasPrefix("--"), index + 1 < arguments.count else { usage() }
    options[String(key.dropFirst(2))] = arguments[index + 1]
    index += 2
}
guard let setPath = options["set"], let modelPath = options["model"] else { usage() }
guard let lattice = BenchmarkRun.Lattice(rawValue: options["lattice"] ?? "off") else { usage() }
let backend = options["backend"] ?? "llama"
guard backend == "llama" || backend == "mlx" else { usage() }
let adapterPath = options["adapter"].flatMap { $0.isEmpty ? nil : $0 }

configureDataDirectory()

var items: [Benchmark.Item]
do {
    items = try Benchmark.loadItems(contentsOf: URL(fileURLWithPath: setPath))
} catch {
    fail("評価セットが読めません: \(error.localizedDescription)")
}
if let limit = options["limit"].flatMap(Int.init), limit > 0, limit < items.count {
    items = Array(items.prefix(limit))
}
guard FileManager.default.fileExists(atPath: modelPath) else {
    fail("モデルがありません: \(modelPath)")
}
let benchmarkID = options["id"] ?? URL(fileURLWithPath: setPath).deletingPathExtension().lastPathComponent
let benchmarkName = options["name"] ?? benchmarkID

// MARK: - エンジン

let model: EngineAssembly.Model
if backend == "mlx" {
    guard MLXConversionEngine.supports(modelPath: modelPath) else {
        fail("このモデルは MLX で動かせません（MLX は T5 のモデルだけ）")
    }
    model = .mlx(MLXConversionEngine(modelPath: modelPath, adapterPath: adapterPath))
} else {
    model = .llamaCpp(ZenzEngine(modelPath: modelPath, adapterPath: adapterPath))
}
let latticeConverter = LatticeConverter.defaultDictionaryURL().map {
    LatticeConverter(dictionaryURL: $0) { TermDictionary.empty }
}
if lattice == .always && latticeConverter == nil {
    fail("辞書ラティスの辞書が見つかりません")
}
let engine = EngineAssembly.make(
    model: model, lattice: latticeConverter, latticeUse: lattice == .always ? .firstCandidate : .off)

// MARK: - 計測

let hostName = LaunchLog.currentHostName()
let thermalBefore = BenchmarkRun.Environment.thermalStateName(ProcessInfo.processInfo.thermalState)
let startedAt = Date()
let started = ContinuousClock.now

emit(BenchmarkRun.Event(kind: .loading, done: 0, total: items.count))
let warmUp = await Benchmark.warmUp(engine: engine)

var results: [Benchmark.ItemResult] = []
results.reserveCapacity(items.count)
var hits = 0
for item in items {
    let result = await Benchmark.run(item, engine: engine)
    results.append(result)
    if result.hit { hits += 1 }
    emit(BenchmarkRun.Event(kind: .progress, done: results.count, total: items.count, hits: hits))
}

let elapsedSeconds = Benchmark.milliseconds(started.duration(to: .now)) / 1000
// 計測中に打ったキーがあれば、その変換と CPU・GPU を取り合っている（権限は要らない）
let secondsSinceKey = CGEventSource.secondsSinceLastEventType(.combinedSessionState, eventType: .keyDown)
let environment = BenchmarkRun.Environment(
    appVersion: appVersion(options["app-version"]),
    hostName: hostName,
    machineModel: sysctlString("hw.model") ?? "?",
    chip: sysctlString("machdep.cpu.brand_string") ?? "?",
    osVersion: ProcessInfo.processInfo.operatingSystemVersionString,
    powerSource: powerSource(),
    lowPowerMode: ProcessInfo.processInfo.isLowPowerModeEnabled,
    thermalStateBefore: thermalBefore,
    thermalStateAfter: BenchmarkRun.Environment.thermalStateName(ProcessInfo.processInfo.thermalState))
let record = BenchmarkRun.Record(
    id: BenchmarkRun.makeID(date: startedAt, hostName: hostName),
    benchmarkID: benchmarkID,
    benchmarkName: benchmarkName,
    condition: BenchmarkRun.Condition(
        lattice: lattice, backend: backend,
        model: URL(fileURLWithPath: modelPath).lastPathComponent,
        adapter: adapterPath.map { URL(fileURLWithPath: $0).lastPathComponent }),
    environment: environment,
    startedAt: startedAt,
    finishedAt: Date(),
    warmUpMilliseconds: warmUp,
    inputDuringRun: secondsSinceKey < elapsedSeconds,
    summary: Benchmark.Summary(results),
    misses: BenchmarkRun.Record.misses(items: items, results: results))
do {
    try BenchmarkRun.write(record, results: results)
} catch {
    fail("記録を書けませんでした: \(error.localizedDescription)")
}
emit(BenchmarkRun.Event(kind: .finished, done: results.count, total: items.count, hits: hits, recordID: record.id))
finish(0)
