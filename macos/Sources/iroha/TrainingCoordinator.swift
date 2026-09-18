import Foundation
import IrohaCore

/// 追加学習ヘルパー `iroha-train`（Contents/MacOS/、MLX をリンク）を別プロセスで動かし、
/// 標準出力の JSON Lines（`TrainingEvent`）を設定画面の状態にする。
/// IME 本体に MLX を入れないのと、学習中の GPU/CPU 占有や異常終了を入力処理から切り離すために別プロセスにする
final class TrainingCoordinator: ObservableObject {
    static let shared = TrainingCoordinator()

    /// `iroha-train info` の出力（`TrainingRun.Summary` と同じキー）
    struct Info: Codable, Equatable {
        var totalEntries: Int
        var usableEntries: Int
        var architecture: String
        var supported: Bool
        var minimumRecords: Int
    }

    /// 件数で進む処理（記録の確認）の進み具合
    struct Progress: Equatable {
        var done: Int
        var total: Int
    }

    enum State: Equatable {
        case idle
        case running(stage: String, step: TrainingStep?, progress: Progress?)
        case done(TrainingResult)
        case failed(String)
        case cancelled
    }

    @Published private(set) var state: State = .idle
    @Published private(set) var info: Info?
    @Published private(set) var infoError: String?

    private var process: Process?
    private var stdoutBuffer = Data()

    /// ヘルパーの実行ファイル（バンドル内。無ければこの機能は使えない）
    static var helperURL: URL? {
        guard let url = Bundle.main.executableURL?.deletingLastPathComponent().appendingPathComponent("iroha-train"),
              FileManager.default.isExecutableFile(atPath: url.path) else { return nil }
        return url
    }

    /// MLX は Apple Silicon のみ
    static var isSupportedHardware: Bool {
        #if arch(arm64)
        return true
        #else
        return false
        #endif
    }

    static var isAvailable: Bool { isSupportedHardware && helperURL != nil }

    /// アダプタの置き場所（データフォルダの models/adapters/）
    static var adaptersDirectory: URL {
        DataDirectory.modelsURL.appendingPathComponent("adapters", isDirectory: true)
    }

    /// ヘルパーの stderr（llama.cpp / MLX のログ）を残す先
    private static var logURL: URL {
        DataDirectory.logsURL.appendingPathComponent("train-\(LaunchLog.currentHostName()).log")
    }

    // MARK: - 記録の集計

    /// `iroha-train info` を回して件数・対応可否を取り直す（MLX には触らない軽い処理）
    func refreshInfo(basePath: String) {
        guard let helper = Self.helperURL else { return }
        let process = Process()
        process.executableURL = helper
        process.arguments = ["info", "--base", basePath]
        process.environment = Self.environment()
        let output = Pipe()
        process.standardOutput = output
        process.standardError = FileHandle.nullDevice
        process.terminationHandler = { [weak self] _ in
            let data = output.fileHandleForReading.readDataToEndOfFile()
            DispatchQueue.main.async {
                if let info = try? JSONDecoder().decode(Info.self, from: data) {
                    self?.info = info
                    self?.infoError = nil
                } else if case .error(let message)? = TrainingEvent.parse(line: String(decoding: data, as: UTF8.self).trimmingCharacters(in: .newlines)) {
                    self?.infoError = message
                } else {
                    self?.infoError = "記録の集計に失敗しました"
                }
            }
        }
        do {
            try process.run()
        } catch {
            infoError = "iroha-train を起動できません: \(error.localizedDescription)"
        }
    }

    // MARK: - 学習

    func start(basePath: String) {
        guard let helper = Self.helperURL, process == nil else { return }
        let baseName = URL(fileURLWithPath: basePath).deletingPathExtension().lastPathComponent
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd-HHmm"
        let outputURL = Self.adaptersDirectory.appendingPathComponent("\(baseName)-lora-\(formatter.string(from: Date())).gguf")
        try? FileManager.default.createDirectory(at: Self.adaptersDirectory, withIntermediateDirectories: true)
        try? FileManager.default.createDirectory(at: DataDirectory.logsURL, withIntermediateDirectories: true)

        let process = Process()
        process.executableURL = helper
        // エポック・学習率は設定画面の値（`TrainingSettings`）。それ以外は `TrainingConfig` の既定
        process.arguments = ["train", "--base", basePath, "--out", outputURL.path,
                             "--epochs", String(TrainingSettings.epochs),
                             "--lr", String(format: "%g", TrainingSettings.learningRate)]
        process.environment = Self.environment()
        let output = Pipe()
        process.standardOutput = output
        if FileManager.default.createFile(atPath: Self.logURL.path, contents: nil),
           let log = try? FileHandle(forWritingTo: Self.logURL) {
            process.standardError = log
        } else {
            process.standardError = FileHandle.nullDevice
        }
        stdoutBuffer = Data()
        output.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { return }
            DispatchQueue.main.async { self?.consume(data) }
        }
        process.terminationHandler = { [weak self] process in
            output.fileHandleForReading.readabilityHandler = nil
            let rest = output.fileHandleForReading.readDataToEndOfFile()
            DispatchQueue.main.async {
                guard let self else { return }
                self.consume(rest)
                self.process = nil
                switch self.state {
                case .done, .failed, .cancelled: break
                case .idle, .running:
                    if process.terminationReason == .uncaughtSignal || process.terminationStatus == 15 {
                        self.state = .cancelled
                    } else {
                        self.state = .failed("学習が途中で終了しました（終了コード \(process.terminationStatus)）。"
                            + "詳細は \(Self.logURL.lastPathComponent) を参照")
                    }
                }
                NSLog("iroha: iroha-train 終了 status=\(process.terminationStatus)")
            }
        }
        state = .running(stage: "start", step: nil, progress: nil)
        do {
            try process.run()
            self.process = process
            NSLog("iroha: iroha-train 開始 base=\(basePath) out=\(outputURL.path)")
        } catch {
            state = .failed("iroha-train を起動できません: \(error.localizedDescription)")
        }
    }

    func cancel() {
        guard let process else { return }
        state = .cancelled
        process.terminate()
    }

    func reset() {
        guard process == nil else { return }
        state = .idle
    }

    // MARK: - 出力の解釈

    private func consume(_ data: Data) {
        stdoutBuffer.append(data)
        while let newline = stdoutBuffer.firstIndex(of: 0x0A) {
            let line = String(decoding: stdoutBuffer[stdoutBuffer.startIndex..<newline], as: UTF8.self)
            stdoutBuffer.removeSubrange(stdoutBuffer.startIndex...newline)
            guard let event = TrainingEvent.parse(line: line) else { continue }
            apply(event)
        }
    }

    private func apply(_ event: TrainingEvent) {
        // キャンセル後に届く進捗は無視
        if case .cancelled = state { return }
        switch event {
        case .data, .eval:
            break
        case .stage(let stage):
            state = .running(stage: stage, step: nil, progress: nil)
        case .progress(let stage, let done, let total):
            state = .running(stage: stage, step: nil, progress: Progress(done: done, total: total))
        case .step(let step):
            state = .running(stage: "train", step: step, progress: nil)
        case .done(let result):
            state = .done(result)
        case .error(let message):
            state = .failed(message)
        }
    }

    private static func environment() -> [String: String] {
        var environment = ProcessInfo.processInfo.environment
        environment["IROHA_DATA_DIR"] = DataDirectory.url.path
        return environment
    }
}
