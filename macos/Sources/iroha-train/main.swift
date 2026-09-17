import Foundation
import CLlama
import IrohaCore
import IrohaTrain

// 変換記録（ConversionLog）から LoRA アダプタを学習するヘルパー。IME 本体（設定画面）が
// Process で起動し、標準出力の JSON Lines（TrainingEvent）で進捗を受け取る。
//
// 使い方:
//   iroha-train info  --base <model.gguf>                       : 記録の件数・対応可否を JSON で出す（MLX に触らない）
//   iroha-train train --base <model.gguf> --out <adapter.gguf>  : 学習して GGUF アダプタを書く
//               [--epochs N] [--rank N] [--alpha F] [--lr F] [--batch N] [--no-eval] [--json]
//   環境変数 IROHA_DATA_DIR でデータフォルダ（記録の場所）を上書き。無ければ IME 本体の設定に従う
//
// MLX の Metal カーネル（mlx-swift_Cmlx.bundle/default.metallib）は実行ファイルと同じ場所か
// .app の Contents/Resources から MLX 自身が探す（scripts/build-mlx-metallib.sh 参照）

setvbuf(stdout, nil, _IOLBF, 0)  // 行ごとに流す（親プロセスが逐次読む）

// llama.cpp のログは stderr へ（stdout は JSON 専用）
llama_log_set({ level, text, _ in
    guard let text, level.rawValue >= GGML_LOG_LEVEL_WARN.rawValue else { return }
    FileHandle.standardError.write(Data(String(cString: text).utf8))
}, nil)

func configureDataDirectory() {
    let env = ProcessInfo.processInfo.environment
    if let path = env["IROHA_DATA_DIR"], !path.isEmpty {
        DataDirectory.configure(URL(fileURLWithPath: path, isDirectory: true))
    } else if let path = UserDefaults(suiteName: "dev.iroha.inputmethod.iroha")?
        .string(forKey: "dataDirectory"), !path.isEmpty {
        DataDirectory.configure(URL(fileURLWithPath: path, isDirectory: true))
    }
}

func fail(_ message: String) -> Never {
    print(TrainingEvent.error(message).jsonLine())
    exit(1)
}

func usage() -> Never {
    FileHandle.standardError.write("""
    使い方:
      iroha-train info  --base <model.gguf>
      iroha-train train --base <model.gguf> --out <adapter.gguf> [--epochs N] [--rank N] [--alpha F] [--lr F] [--batch N] [--no-eval]

    """.data(using: .utf8)!)
    exit(2)
}

struct Arguments {
    var command = ""
    var options: [String: String] = [:]
    var flags: Set<String> = []

    init(_ arguments: [String]) {
        guard arguments.count >= 2 else { return }
        command = arguments[1]
        var index = 2
        while index < arguments.count {
            let argument = arguments[index]
            guard argument.hasPrefix("--") else { usage() }
            let key = String(argument.dropFirst(2))
            if index + 1 < arguments.count, !arguments[index + 1].hasPrefix("--") {
                options[key] = arguments[index + 1]
                index += 2
            } else {
                flags.insert(key)
                index += 1
            }
        }
    }

    func int(_ key: String) -> Int? { options[key].flatMap(Int.init) }
    func float(_ key: String) -> Float? { options[key].flatMap(Float.init) }
}

/// SIGTERM / SIGINT を受けたら真（シグナルハンドラは文脈を捕捉できないのでグローバル）
nonisolated(unsafe) var stopRequested = false

configureDataDirectory()
let arguments = Arguments(CommandLine.arguments)
guard let basePath = arguments.options["base"] else { usage() }

switch arguments.command {
case "info":
    do {
        let summary = try TrainingRun.summarize(basePath: basePath)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        print(String(data: try encoder.encode(summary), encoding: .utf8) ?? "{}")
    } catch {
        fail("\(error)")
    }

case "train":
    guard let outputPath = arguments.options["out"] else { usage() }
    var options = TrainingRun.Options(basePath: basePath, outputPath: outputPath)
    options.skipEvaluation = arguments.flags.contains("no-eval")
    if arguments.options.keys.contains(where: { ["epochs", "rank", "alpha", "lr", "batch"].contains($0) }) {
        var config = TrainingConfig.recommended(forExampleCount: (try? TrainingRun.summarize(basePath: basePath).usableEntries) ?? 0)
        if let value = arguments.int("epochs") { config.epochs = value }
        if let value = arguments.int("rank") { config.rank = value }
        if let value = arguments.float("alpha") { config.alpha = value }
        if let value = arguments.float("lr") { config.learningRate = value }
        if let value = arguments.int("batch") { config.batchSize = value }
        options.config = config
    }

    // SIGTERM（親のキャンセル）で次のステップ境界で止める。部分ファイルは残さない
    signal(SIGTERM) { _ in stopRequested = true }
    signal(SIGINT) { _ in stopRequested = true }

    do {
        try await TrainingRun.run(options, emit: { print($0.jsonLine()) }, shouldStop: { stopRequested })
    } catch {
        try? FileManager.default.removeItem(atPath: outputPath + ".tmp")
        fail("\(error)")
    }

default:
    usage()
}
