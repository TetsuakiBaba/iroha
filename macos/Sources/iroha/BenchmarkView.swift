import AppKit
import IrohaCore
import SwiftUI

/// 設定 > モデル > 変換のベンチマーク（開発者モード）。
///
/// 評価セットを取得し、今動いているモデルで 1 件ずつ変換して精度（acc@1・MinCER）と 1 回の変換の時間を測る。
/// 計測は別プロセス `iroha-bench`（`BenchmarkCoordinator`）。結果は `<データフォルダ>/logs/bench/` に残る
struct BenchmarkSection: View {
    @ObservedObject private var coordinator = BenchmarkCoordinator.shared
    @AppStorage("benchmarkSelectedSet") private var selectedSet = ""
    @AppStorage("benchmarkLattice") private var latticeRaw = BenchmarkRun.Lattice.off.rawValue
    /// 開いている結果（nil なら直近のもの）
    @State private var shownRecordID: String?

    private var lattice: BenchmarkRun.Lattice { BenchmarkRun.Lattice(rawValue: latticeRaw) ?? .off }

    /// 選べる評価セット（入れてあるもの）。選んだものが無ければ先頭
    private var selectedRecord: BenchmarkInstall.Record? {
        coordinator.installed.first { $0.id == selectedSet } ?? coordinator.installed.first
    }

    private var shownRecord: BenchmarkRun.Record? {
        if let id = shownRecordID, let record = coordinator.records.first(where: { $0.id == id }) { return record }
        return coordinator.records.first
    }

    var body: some View {
        Section {
            if BenchmarkCoordinator.helperURL == nil {
                Text("計測ヘルパー（iroha-bench）がこのバンドルにありません。").foregroundStyle(.secondary)
            } else {
                setRows
                conditionRows
                runRow
                if let record = shownRecord {
                    BenchmarkResultView(record: record)
                }
                if coordinator.records.count > 1 {
                    historyRows
                }
            }
        } header: {
            HelpSectionHeader(
                title: "変換のベンチマーク",
                help: "評価セットを 1 件ずつ、今動いているモデル・推論エンジン・アダプタで変換し、"
                    + "正解との一致（acc@1 = 正解のどれかと完全に一致した割合、MinCER = 正解との文字の違いの割合）と、"
                    + "1 回の変換にかかった時間を測ります。"
                    + "ユーザ辞書・学習・自分の原稿の表記・専門辞書は使いません（ほかの Mac の結果と比べられるようにするため）。"
                    + "条件の「モデルだけ」はライブ変換の第一候補と同じ経路、「辞書ラティス + 再採点」は辞書の候補を"
                    + "モデルで採点し直して選ぶ経路です。"
                    + "計測は別のプロセス（iroha-bench）で行うので計測中も入力できますが、そのあいだの速度の数字は"
                    + "入力の変換と取り合った値になります（結果に印が付きます）。ノートの Mac は電源につないで測ってください。"
                    + "結果はデータフォルダの logs/bench/ に残ります。.tsv は testdata/speed/summarize.py でそのまま集計できます。"
                    + "評価セットは「取得」を押したときにダウンロードします（iroha-bench-v1 は CC BY-SA 4.0）。",
                developer: true)
        }
        .onAppear {
            coordinator.refresh()
            coordinator.refreshCatalogIfNeeded()
        }
        .onChange(of: coordinator.records.first?.id) { _, _ in shownRecordID = nil }
    }

    // MARK: - 評価セット

    @ViewBuilder private var setRows: some View {
        if let catalog = coordinator.catalog {
            ForEach(catalog.benchmarks) { entry in
                catalogRow(entry)
            }
        } else if let error = coordinator.catalogError {
            Text("評価セットの一覧を取得できませんでした: \(error)").font(.caption).foregroundStyle(.red)
        } else {
            LabeledContent("評価セットの一覧") { ProgressView().controlSize(.small) }
        }
        // カタログから消えた・オフラインでも、入れてあるものは消せるようにする
        ForEach(coordinator.installed.filter { coordinator.catalog?.entry(id: $0.id) == nil }) { record in
            LabeledContent {
                Button("削除") { coordinator.remove(id: record.id) }.disabled(coordinator.isRunning)
            } label: {
                Text("\(record.name)（\(record.items) 件）")
            }
        }
        if let error = coordinator.installError {
            Text("取得できませんでした: \(error)").font(.caption).foregroundStyle(.red)
        }
    }

    @ViewBuilder private func catalogRow(_ entry: BenchmarkCatalog.Entry) -> some View {
        let installed = coordinator.installed.first { $0.id == entry.id }
        LabeledContent {
            HStack {
                if coordinator.installing.contains(entry.id) {
                    ProgressView().controlSize(.small)
                } else if let installed {
                    if coordinator.isOutdated(installed) {
                        Button("更新") { coordinator.install(entry) }
                    }
                    Button("削除") { coordinator.remove(id: entry.id) }
                        .disabled(coordinator.isRunning)
                } else {
                    Button("取得（\(ByteCountFormatter.string(fromByteCount: Int64(entry.file.bytes), countStyle: .file))）") {
                        coordinator.install(entry)
                    }
                }
            }
        } label: {
            if let summary = entry.summary {
                HelpLabel(title: "\(entry.name)（\(entry.items) 件）", help: summary)
            } else {
                Text("\(entry.name)（\(entry.items) 件）")
            }
        }
    }

    // MARK: - 条件と開始

    @ViewBuilder private var conditionRows: some View {
        if coordinator.installed.count > 1 {
            Picker("測る評価セット", selection: Binding(
                get: { selectedRecord?.id ?? "" }, set: { selectedSet = $0 })
            ) {
                ForEach(coordinator.installed) { Text($0.name).tag($0.id) }
            }
            .disabled(coordinator.isRunning)
        }
        Picker("条件", selection: $latticeRaw) {
            ForEach(BenchmarkRun.Lattice.allCases, id: \.rawValue) { Text($0.displayName).tag($0.rawValue) }
        }
        .disabled(coordinator.isRunning)
        LabeledContent("測るモデル") {
            Text(IrohaInputController.engineModelName + "（\(IrohaInputController.engineBackend.displayName)）")
                .foregroundStyle(.secondary)
        }
    }

    @ViewBuilder private var runRow: some View {
        switch coordinator.state {
        case .idle:
            HStack {
                Button("計測を開始") {
                    if let record = selectedRecord { coordinator.start(benchmarkID: record.id, lattice: lattice) }
                }
                .disabled(selectedRecord == nil)
                if selectedRecord == nil {
                    Text("先に評価セットを取得してください。").font(.caption).foregroundStyle(.secondary)
                }
            }
        case .loading:
            HStack {
                ProgressView().controlSize(.small)
                Text("モデルを読み込み中…").foregroundStyle(.secondary)
                Spacer()
                Button("中止") { coordinator.cancel() }
            }
        case .running(let done, let total, let hits):
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    ProgressView(value: Double(done), total: Double(max(total, 1)))
                    Button("中止") { coordinator.cancel() }
                }
                Text("\(done)/\(total) 件（正解 \(hits) 件）。計測中に入力すると、速度の数字が乱れます。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        case .failed(let message):
            VStack(alignment: .leading, spacing: 4) {
                Text("計測に失敗しました: \(message)").font(.caption).foregroundStyle(.red)
                Button("戻る") { coordinator.reset() }
            }
        case .cancelled:
            HStack {
                Text("計測を中止しました。").foregroundStyle(.secondary)
                Button("戻る") { coordinator.reset() }
            }
        }
    }

    // MARK: - 過去の結果

    @ViewBuilder private var historyRows: some View {
        DisclosureGroup("過去の結果（\(coordinator.records.count) 件）") {
            ForEach(coordinator.records) { record in
                HStack {
                    Button {
                        shownRecordID = record.id
                    } label: {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(BenchmarkResultView.title(record))
                                .fontWeight(record.id == shownRecord?.id ? .semibold : .regular)
                            Text(String(format: "acc@1 %.1f%%・中央値 %.1f ms", record.summary.total.accuracy * 100,
                                        record.summary.latency.p50)
                                + "・\(record.environment.chip)（\(record.environment.hostName)）")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                    .buttonStyle(.plain)
                    Spacer()
                    Button("削除") { coordinator.deleteRecord(id: record.id) }
                }
            }
        }
    }
}

/// 1 回の計測の結果（精度の表・速度の表・読みの長さ別の中央値）
struct BenchmarkResultView: View {
    let record: BenchmarkRun.Record

    static func title(_ record: BenchmarkRun.Record) -> String {
        let date = DateFormatter.localizedString(from: record.startedAt, dateStyle: .short, timeStyle: .short)
        let model = URL(fileURLWithPath: record.condition.model).deletingPathExtension().lastPathComponent
        return "\(date)  \(record.benchmarkName)・\(record.condition.lattice.displayName)・\(model)"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(Self.title(record)).fontWeight(.medium)
            if record.inputDuringRun {
                Text("計測中にキーボードの入力がありました。速度の数字は入力の変換と取り合った値です。")
                    .font(.caption)
                    .foregroundStyle(.orange)
            }
            if record.environment.powerSource == "battery" || record.environment.lowPowerMode
                || record.environment.thermalStateAfter != "nominal" {
                Text("電源 \(record.environment.powerSource == "battery" ? "バッテリー" : "AC")"
                    + (record.environment.lowPowerMode ? "・低電力モード" : "")
                    + "・発熱の状態 \(record.environment.thermalStateBefore) → \(record.environment.thermalStateAfter)。"
                    + "速度の数字はこの影響を受けています。")
                    .font(.caption)
                    .foregroundStyle(.orange)
            }
            accuracyTable
            latencyTable
            HStack {
                Button("Markdown でコピー") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(BenchmarkRun.markdown(record), forType: .string)
                }
                Button("記録のフォルダを開く") {
                    NSWorkspace.shared.activateFileViewerSelecting([BenchmarkRun.recordURL(id: record.id)])
                }
                if !record.misses.isEmpty {
                    BenchmarkMissesButton(record: record)
                }
            }
        }
        .padding(.vertical, 4)
    }

    private var accuracyTable: some View {
        Grid(alignment: .trailing, horizontalSpacing: 16, verticalSpacing: 2) {
            GridRow {
                Text("区分").gridColumnAlignment(.leading)
                Text("件数")
                Text("acc@1")
                Text("MinCER")
            }
            .foregroundStyle(.secondary)
            accuracyRow("全体", record.summary.total, bold: true)
            accuracyRow("文脈あり", record.summary.withContext)
            accuracyRow("文脈なし", record.summary.withoutContext)
            ForEach(record.summary.perSource, id: \.source) { accuracyRow($0.source, $0.tally) }
        }
        .font(.callout.monospacedDigit())
    }

    @ViewBuilder private func accuracyRow(_ label: String, _ tally: Benchmark.Tally, bold: Bool = false) -> some View {
        if tally.count > 0 {
            GridRow {
                Text(label).gridColumnAlignment(.leading)
                Text("\(tally.count)")
                Text(String(format: "%.1f%%", tally.accuracy * 100))
                Text(String(format: "%.2f%%", tally.meanMinCER * 100))
            }
            .fontWeight(bold ? .semibold : .regular)
        }
    }

    private var latencyTable: some View {
        let latency = record.summary.latency
        return VStack(alignment: .leading, spacing: 4) {
            Grid(alignment: .trailing, horizontalSpacing: 16, verticalSpacing: 2) {
                GridRow {
                    Text("平均"); Text("中央値"); Text("90%"); Text("99%"); Text("最大"); Text("NN の割合")
                }
                .foregroundStyle(.secondary)
                GridRow {
                    Text(String(format: "%.1f", latency.mean))
                    Text(String(format: "%.1f", latency.p50))
                    Text(String(format: "%.1f", latency.p90))
                    Text(String(format: "%.1f", latency.p99))
                    Text(String(format: "%.1f", latency.max))
                    Text(record.summary.neuralNetworkShare.map { String(format: "%.0f%%", $0 * 100) } ?? "—")
                }
            }
            Grid(alignment: .trailing, horizontalSpacing: 16, verticalSpacing: 2) {
                GridRow {
                    Text("読みの文字数").gridColumnAlignment(.leading)
                    ForEach(record.summary.lengthBins, id: \.lower) { Text($0.label) }
                }
                .foregroundStyle(.secondary)
                GridRow {
                    Text("中央値").gridColumnAlignment(.leading)
                    ForEach(record.summary.lengthBins, id: \.lower) { Text(String(format: "%.1f", $0.p50)) }
                }
            }
            Text("単位はミリ秒（1 件 = 読み 1 つの変換 1 回）。モデルの読み込みを含む最初の変換 "
                + String(format: "%.0f", record.warmUpMilliseconds) + " ms は除いています。")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .font(.callout.monospacedDigit())
    }
}

/// 正解しなかった項目の一覧（ポップオーバー）
private struct BenchmarkMissesButton: View {
    let record: BenchmarkRun.Record
    @State private var isPresented = false

    var body: some View {
        Button("間違えた項目（\(record.misses.count) 件）") { isPresented = true }
            .popover(isPresented: $isPresented, arrowEdge: .bottom) {
                List(record.misses, id: \.index) { miss in
                    VStack(alignment: .leading, spacing: 2) {
                        Text(miss.output).fontWeight(.medium)
                        Text("正解: \(miss.expected.joined(separator: " / "))").foregroundStyle(.secondary)
                        Text("読み: \(miss.input)" + (miss.context.isEmpty ? "" : "　文脈: \(miss.context)"))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Text(miss.index).font(.caption2).foregroundStyle(.tertiary)
                    }
                    .textSelection(.enabled)
                }
                .frame(width: 520, height: 420)
            }
    }
}
