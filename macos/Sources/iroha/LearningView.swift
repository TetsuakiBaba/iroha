import IrohaCore
import SwiftUI

/// 変換の学習（learning.json）の確認・編集画面（設定ウィンドウからシートで開く）。
///
/// 学習は変換のたびに引かれる辞書なので、覚え違いが残っていると同じ誤変換が出続ける。
/// ここで中身を見て直す・消すことができる。変更は `LearningStore` に即時保存され、次の変換から反映される
struct LearningView: View {
    @Environment(\.dismiss) private var dismiss

    /// 一覧の1行（`LearningEntry` は識別子を持たないのでここで付ける）
    private struct Row: Identifiable {
        let id = UUID()
        var entry: LearningEntry
    }

    @State private var rows: [Row] = []
    @State private var searchText = ""
    @State private var showingResetConfirmation = false

    private var filtered: [Row] {
        let query = searchText.trimmingCharacters(in: .whitespaces)
        guard !query.isEmpty else { return rows }
        return rows.filter {
            $0.entry.reading.contains(query) || $0.entry.result.contains(query)
        }
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
        // 新しく覚えたものから見せる（直したいのは直近の覚え違い）
        .onAppear {
            rows = LearningStore.shared.current.entries
                .sorted { $0.updatedAt > $1.updatedAt }
                .map(Row.init)
        }
    }

    private var header: some View {
        HStack {
            Text("変換の学習").font(.headline)
            Spacer()
            TextField("検索", text: $searchText)
                .textFieldStyle(.roundedBorder)
                .frame(width: 160)
        }
        .padding(12)
    }

    private var list: some View {
        Group {
            if rows.isEmpty {
                VStack(spacing: 8) {
                    Text("学習した変換はありません").foregroundStyle(.secondary)
                    Text("文節変換（スペースキー）で候補を選び直して確定すると、その読み全体の変換を覚えます。")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List {
                    HStack(spacing: 8) {
                        Text("よみ").frame(width: 220, alignment: .leading)
                        Text("変換結果").frame(maxWidth: .infinity, alignment: .leading)
                        Spacer().frame(width: 20)
                    }
                    .font(.caption)
                    .foregroundStyle(.secondary)

                    ForEach(filtered) { row in
                        self.row(for: row)
                    }
                }
                .listStyle(.inset)
            }
        }
    }

    private func row(for row: Row) -> some View {
        HStack(spacing: 8) {
            TextField("よみ", text: binding(for: row.id, keyPath: \.reading))
                .frame(width: 220)
                .help("この読みを丸ごと入力したときに、右の変換結果を返します")
            TextField("変換結果", text: binding(for: row.id, keyPath: \.result))
                .frame(maxWidth: .infinity)
            Button {
                rows.removeAll { $0.id == row.id }
                save()
            } label: {
                Image(systemName: "minus.circle")
            }
            .buttonStyle(.borderless)
            .help("この学習を削除")
        }
        .textFieldStyle(.plain)
    }

    private var footer: some View {
        HStack {
            Button("Finderで表示") {
                let url = LearningStore.defaultURL
                if FileManager.default.fileExists(atPath: url.path) {
                    NSWorkspace.shared.activateFileViewerSelecting([url])
                } else {
                    NSWorkspace.shared.open(url.deletingLastPathComponent())
                }
            }
            Button("すべてリセット...") { showingResetConfirmation = true }
                .disabled(rows.isEmpty)
            Spacer()
            Text(filtered.count == rows.count ? "\(rows.count) 件" : "\(filtered.count) / \(rows.count) 件")
                .foregroundStyle(.secondary)
                .font(.caption)
            Button("閉じる") {
                save()
                dismiss()
            }
            .keyboardShortcut(.defaultAction)
        }
        .padding(12)
        .confirmationDialog("学習した変換をすべて消しますか？", isPresented: $showingResetConfirmation,
                            titleVisibility: .visible) {
            Button("リセット", role: .destructive) {
                LearningStore.shared.reset()
                rows = []
            }
        } message: {
            Text("覚えた変換（learning.json）を削除します。この操作は取り消せません。")
        }
    }

    private func binding(for id: UUID, keyPath: WritableKeyPath<LearningEntry, String>) -> Binding<String> {
        Binding(
            get: { rows.first { $0.id == id }?.entry[keyPath: keyPath] ?? "" },
            set: { newValue in
                guard let index = rows.firstIndex(where: { $0.id == id }) else { return }
                rows[index].entry[keyPath: keyPath] = newValue
                // 直した内容を新しい学習として扱う（同じ読み・文脈の古い記録より優先される）
                rows[index].entry.updatedAt = Date()
                save()
            })
    }

    private func save() {
        LearningStore.shared.replaceAll(rows.map(\.entry))
    }
}
