import IrohaCore
import SwiftUI

/// 入力履歴（予測変換の候補）の確認・削除画面（設定ウィンドウからシートで開く）。
///
/// 予測は確定した回数の多い順に出るので、覚え違いや出したくない語が上に来ることがある。
/// ここで消すと、同じデータフォルダを使う他のMacで確定した分も数えなくなる（`InputHistoryStore.remove`）。
/// 消したあとにまた確定すれば、その分から数え直す
struct InputHistoryView: View {
    @Environment(\.dismiss) private var dismiss

    @State private var entries: [InputHistoryEntry] = []
    @State private var searchText = ""
    @State private var showingResetConfirmation = false

    private static let dateFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy/MM/dd HH:mm"
        return formatter
    }()

    private var filtered: [InputHistoryEntry] {
        let query = searchText.trimmingCharacters(in: .whitespaces)
        guard !query.isEmpty else { return entries }
        return entries.filter { $0.reading.contains(query) || $0.text.contains(query) }
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            list
            Divider()
            footer
        }
        .frame(width: 640, height: 460)
        .onAppear(perform: reload)
    }

    /// 予測に出る順（回数の多い順、同じなら最近使った順）
    private func reload() {
        entries = InputHistoryStore.shared.entries.sorted {
            $0.count != $1.count ? $0.count > $1.count : $0.lastUsed > $1.lastUsed
        }
    }

    private var header: some View {
        HStack {
            Text("入力履歴").font(.headline)
            Spacer()
            TextField("検索", text: $searchText)
                .textFieldStyle(.roundedBorder)
                .frame(width: 160)
        }
        .padding(12)
    }

    private var list: some View {
        Group {
            if entries.isEmpty {
                VStack(spacing: 8) {
                    Text("入力履歴はありません").foregroundStyle(.secondary)
                    Text("予測変換がONの間、ローマ字で入力して確定した語を覚えます。")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List {
                    HStack(spacing: 8) {
                        Text("よみ").frame(width: 180, alignment: .leading)
                        Text("表記").frame(maxWidth: .infinity, alignment: .leading)
                        Text("回数").frame(width: 50, alignment: .trailing)
                        Text("最後に確定").frame(width: 120, alignment: .leading)
                        Spacer().frame(width: 20)
                    }
                    .font(.caption)
                    .foregroundStyle(.secondary)

                    ForEach(filtered, id: \.key) { entry in
                        HStack(spacing: 8) {
                            Text(entry.reading).frame(width: 180, alignment: .leading)
                            Text(entry.text).frame(maxWidth: .infinity, alignment: .leading)
                            Text("\(entry.count)")
                                .monospacedDigit()
                                .frame(width: 50, alignment: .trailing)
                            Text(Self.dateFormatter.string(from: entry.lastUsed))
                                .foregroundStyle(.secondary)
                                .frame(width: 120, alignment: .leading)
                            Button {
                                InputHistoryStore.shared.remove([entry.key])
                                entries.removeAll { $0.key == entry.key }
                            } label: {
                                Image(systemName: "minus.circle")
                            }
                            .buttonStyle(.borderless)
                            .help("この語を予測に出さない（また確定すれば数え直す）")
                        }
                        .lineLimit(1)
                    }
                }
                .listStyle(.inset)
            }
        }
    }

    private var footer: some View {
        HStack {
            Button("Finderで表示") {
                let url = InputHistoryStore.shared.fileURL(forHost: InputHistoryStore.shared.hostName)
                if FileManager.default.fileExists(atPath: url.path) {
                    NSWorkspace.shared.activateFileViewerSelecting([url])
                } else {
                    let dir = url.deletingLastPathComponent()
                    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                    NSWorkspace.shared.open(dir)
                }
            }
            Button("すべて削除...") { showingResetConfirmation = true }
                .disabled(entries.isEmpty)
            Spacer()
            Text(filtered.count == entries.count
                 ? "\(entries.count) 語" : "\(filtered.count) / \(entries.count) 語")
                .foregroundStyle(.secondary)
                .font(.caption)
            Button("閉じる") { dismiss() }
                .keyboardShortcut(.defaultAction)
        }
        .padding(12)
        .confirmationDialog("入力履歴をすべて削除しますか？", isPresented: $showingResetConfirmation,
                            titleVisibility: .visible) {
            Button("削除", role: .destructive) {
                InputHistoryStore.shared.reset()
                entries = []
            }
        } message: {
            Text("同じデータフォルダを使う他のMacで確定した分も、今より前のものは予測に出なくなります。"
                 + "この操作は取り消せません。")
        }
    }
}
