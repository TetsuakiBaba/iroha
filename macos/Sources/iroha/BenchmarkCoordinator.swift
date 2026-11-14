import Foundation
import IrohaCore

/// 変換のベンチマーク（設定 > モデル > 変換のベンチマーク、開発者モード）。
///
/// 評価セットの取得と、計測ヘルパー `iroha-bench`（Contents/MacOS/）の起動を受け持つ。計測は別プロセスで行い、
/// 標準出力の JSON Lines（`BenchmarkRun.Event`）を設定画面の状態にする（`TrainingCoordinator` と同じ形）。
/// 別プロセスにするのは、計測中も入力を止めないため、MLX のメモリや異常終了を IME から切り離すため
final class BenchmarkCoordinator: ObservableObject {
    static let shared = BenchmarkCoordinator()

    enum State: Equatable {
        case idle
        /// モデルの読み込み（最初の 1 回の変換）
        case loading(total: Int)
        case running(done: Int, total: Int, hits: Int)
        case failed(String)
        case cancelled
    }

    @Published private(set) var state: State = .idle
    @Published private(set) var catalog: BenchmarkCatalog?
    @Published private(set) var catalogError: String?
    @Published private(set) var installed: [BenchmarkInstall.Record] = []
    /// 取得中の評価セットの ID
    @Published private(set) var installing: Set<String> = []
    @Published private(set) var installError: String?
    /// 残っている記録（新しい順）
    @Published private(set) var records: [BenchmarkRun.Record] = []

    private var process: Process?
    private var stdoutBuffer = Data()

    /// ヘルパーの実行ファイル（バンドル内。無ければこの機能は使えない）
    static var helperURL: URL? {
        guard let url = Bundle.main.executableURL?.deletingLastPathComponent().appendingPathComponent("iroha-bench"),
              FileManager.default.isExecutableFile(atPath: url.path) else { return nil }
        return url
    }

    /// ヘルパーの stderr（llama.cpp / MLX のログ）を残す先
    private static var logURL: URL {
        DataDirectory.logsURL.appendingPathComponent("bench-\(LaunchLog.currentHostName()).log")
    }

    var isRunning: Bool { process != nil }

    // MARK: - 評価セット

    /// 入れている評価セットと記録を読み直す（ファイルを読むだけ）
    func refresh() {
        let directory = DataDirectory.url
        DispatchQueue.global(qos: .userInitiated).async {
            let installed = BenchmarkInstall.installedRecords()
            let records = BenchmarkRun.records()
            DispatchQueue.main.async {
                guard DataDirectory.url == directory else { return }
                self.installed = installed
                self.records = records
            }
        }
    }

    /// 評価セットの一覧を取る（設定画面を開いたとき。取れていれば取り直さない）
    func refreshCatalogIfNeeded() {
        guard catalog == nil else { return }
        Task {
            do {
                let catalog = try await BenchmarkFetcher.fetchCatalog()
                await MainActor.run {
                    self.catalog = catalog
                    self.catalogError = nil
                }
            } catch {
                await MainActor.run { self.catalogError = error.localizedDescription }
            }
        }
    }

    func install(_ entry: BenchmarkCatalog.Entry) {
        guard !installing.contains(entry.id) else { return }
        installing.insert(entry.id)
        installError = nil
        Task {
            do {
                try await BenchmarkFetcher.install(entry)
                await MainActor.run {
                    self.installing.remove(entry.id)
                    self.refresh()
                }
            } catch {
                await MainActor.run {
                    self.installing.remove(entry.id)
                    self.installError = "\(entry.name): \(error.localizedDescription)"
                }
            }
        }
    }

    func remove(id: String) {
        try? BenchmarkInstall.remove(id: id)
        refresh()
    }

    /// 入れている版がカタログと違う（作り直された）か
    func isOutdated(_ record: BenchmarkInstall.Record) -> Bool {
        guard let entry = catalog?.entry(id: record.id) else { return false }
        return entry.file.sha256.lowercased() != record.sha256
    }

    // MARK: - 計測

    /// 今動いているモデル・推論エンジン・アダプタで、入れてある評価セットを測る
    func start(benchmarkID: String, lattice: BenchmarkRun.Lattice) {
        guard let helper = Self.helperURL, process == nil,
              let record = installed.first(where: { $0.id == benchmarkID }) else { return }
        try? FileManager.default.createDirectory(at: DataDirectory.logsURL, withIntermediateDirectories: true)

        var arguments = [
            "run",
            "--set", BenchmarkInstall.fileURL(id: record.id).path,
            "--id", record.id,
            "--name", record.name,
            "--model", IrohaInputController.engineModelPath,
            "--backend", IrohaInputController.engineBackend.rawValue,
            "--lattice", lattice.rawValue,
            "--app-version", Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?",
        ]
        if let adapter = IrohaInputController.engineAdapterPath {
            arguments += ["--adapter", adapter]
        }
        let process = Process()
        process.executableURL = helper
        process.arguments = arguments
        var environment = ProcessInfo.processInfo.environment
        environment["IROHA_DATA_DIR"] = DataDirectory.url.path
        process.environment = environment
        // 計測中も入力の変換を優先させる（数字は少し遅くなるが、入力が止まらない）
        process.qualityOfService = .userInitiated
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
                case .idle, .failed, .cancelled: break
                case .loading, .running:
                    if process.terminationReason == .uncaughtSignal || process.terminationStatus == 15 {
                        self.state = .cancelled
                    } else {
                        self.state = .failed("計測が途中で終了しました（終了コード \(process.terminationStatus)）。"
                            + "詳細は logs/\(Self.logURL.lastPathComponent) を参照")
                    }
                }
                NSLog("iroha: iroha-bench 終了 status=\(process.terminationStatus)")
            }
        }
        state = .loading(total: record.items)
        do {
            try process.run()
            self.process = process
            NSLog("iroha: iroha-bench 開始 \(arguments.joined(separator: " "))")
        } catch {
            state = .failed("iroha-bench を起動できません: \(error.localizedDescription)")
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

    func deleteRecord(id: String) {
        BenchmarkRun.delete(id: id)
        records.removeAll { $0.id == id }
    }

    // MARK: - 出力の解釈

    private func consume(_ data: Data) {
        stdoutBuffer.append(data)
        while let newline = stdoutBuffer.firstIndex(of: 0x0A) {
            let line = String(decoding: stdoutBuffer[stdoutBuffer.startIndex..<newline], as: UTF8.self)
            stdoutBuffer.removeSubrange(stdoutBuffer.startIndex...newline)
            guard let event = BenchmarkRun.Event.parse(line) else { continue }
            apply(event)
        }
    }

    private func apply(_ event: BenchmarkRun.Event) {
        if case .cancelled = state { return }
        switch event.kind {
        case .loading:
            state = .loading(total: event.total ?? 0)
        case .progress:
            state = .running(done: event.done ?? 0, total: event.total ?? 0, hits: event.hits ?? 0)
        case .finished:
            // 新しい記録が一覧の先頭に来る（`BenchmarkSection` が先頭を「直近の結果」として開く）
            state = .idle
            refresh()
        case .failed:
            state = .failed(event.message ?? "計測に失敗しました")
        }
    }
}
