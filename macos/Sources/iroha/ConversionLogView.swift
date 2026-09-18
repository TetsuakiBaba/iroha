import IrohaCore
import SwiftUI

/// 変換記録の確認・編集画面（設定ウィンドウからシートで開く）。
///
/// 記録は追加学習（設定 > モデル > 自分の入力で追加学習）の材料になるので、
/// 何が残っているかを確かめ、打ち間違いをそのまま確定した行を直したり消したりできるようにする。
/// 編集は `ConversionLog` が該当ファイル（JSONL）を書き戻す
struct ConversionLogView: View {
    @Environment(\.dismiss) private var dismiss

    @State private var records: [ConversionLog.Record] = []
    @State private var dirty: Set<String> = []
    @State private var searchText = ""
    @State private var correctionsOnly = false
    @State private var showingDeleteAllConfirmation = false

    private var filtered: [ConversionLog.Record] {
        let query = searchText.trimmingCharacters(in: .whitespaces)
        return records.filter { record in
            if correctionsOnly, record.entry.edited == false { return false }
            guard !query.isEmpty else { return true }
            return record.entry.reading.contains(query) || record.entry.committed.contains(query)
                || record.entry.context.contains(query)
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
        .frame(width: 720, height: 480)
        .onAppear { records = ConversionLog.shared.records().reversed() }
        .onDisappear { flush() }
    }

    private var header: some View {
        HStack {
            Text("変換記録").font(.headline)
            Toggle("直した確定だけ", isOn: $correctionsOnly)
                .toggleStyle(.checkbox)
                .padding(.leading, 8)
            Spacer()
            TextField("検索", text: $searchText)
                .textFieldStyle(.roundedBorder)
                .frame(width: 160)
        }
        .padding(12)
    }

    private var list: some View {
        Group {
            if records.isEmpty {
                VStack(spacing: 8) {
                    Text("記録はありません").foregroundStyle(.secondary)
                    Text("「確定した変換を記録する」をONにすると、左文脈のある確定が1件ずつ残ります。")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List {
                    HStack(spacing: 8) {
                        Text("左文脈").frame(width: 200, alignment: .leading)
                        Text("読み").frame(width: 130, alignment: .leading)
                        Text("モデルの出力").frame(width: 130, alignment: .leading)
                        Text("確定").frame(maxWidth: .infinity, alignment: .leading)
                        Spacer().frame(width: 20)
                    }
                    .font(.caption)
                    .foregroundStyle(.secondary)

                    ForEach(filtered) { record in
                        row(for: record)
                    }
                }
                .listStyle(.inset)
            }
        }
    }

    private func row(for record: ConversionLog.Record) -> some View {
        HStack(spacing: 8) {
            // 文脈は長いので末尾だけ見せる（全文はツールチップ）
            Text(record.entry.context.isEmpty ? "—" : "…" + String(record.entry.context.suffix(14)))
                .lineLimit(1)
                .truncationMode(.head)
                .foregroundStyle(.secondary)
                .frame(width: 200, alignment: .leading)
                .help(record.entry.context)
            TextField("読み", text: binding(for: record.id, keyPath: \.reading))
                .frame(width: 130)
                .onSubmit { flush() }
            Text(record.entry.proposed ?? "—")
                .lineLimit(1)
                .foregroundStyle(record.entry.edited == true ? .orange : .secondary)
                .frame(width: 130, alignment: .leading)
                .help(record.entry.edited == true ? "あなたが直した確定" : "そのまま確定")
            TextField("確定", text: binding(for: record.id, keyPath: \.committed))
                .frame(maxWidth: .infinity)
                .onSubmit { flush() }
            Button {
                delete(record)
            } label: {
                Image(systemName: "minus.circle")
            }
            .buttonStyle(.borderless)
            .help("この記録を削除")
        }
        .textFieldStyle(.plain)
    }

    private var footer: some View {
        HStack {
            Button("Finderで表示") {
                let dir = ConversionLog.shared.directory
                try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                NSWorkspace.shared.open(dir)
            }
            Button("すべて削除...") { showingDeleteAllConfirmation = true }
                .disabled(records.isEmpty)
            Spacer()
            Text(filtered.count == records.count
                 ? "\(records.count) 件" : "\(filtered.count) / \(records.count) 件")
                .foregroundStyle(.secondary)
                .font(.caption)
            Button("閉じる") {
                flush()
                dismiss()
            }
            .keyboardShortcut(.defaultAction)
        }
        .padding(12)
        .confirmationDialog("記録した変換をすべて削除しますか？", isPresented: $showingDeleteAllConfirmation,
                            titleVisibility: .visible) {
            Button("削除", role: .destructive) {
                ConversionLog.shared.removeAll()
                records = []
                dirty = []
            }
        } message: {
            Text("logs/conversions/ 内のファイルを削除します。この操作は取り消せません。")
        }
    }

    private func binding(for id: String, keyPath: WritableKeyPath<ConversionLogEntry, String>) -> Binding<String> {
        Binding(
            get: { records.first { $0.id == id }?.entry[keyPath: keyPath] ?? "" },
            set: { newValue in
                guard let index = records.firstIndex(where: { $0.id == id }) else { return }
                records[index].entry[keyPath: keyPath] = newValue
                // 確定を直したら「直した確定か」も付け直す（学習の選り分けに使う）
                records[index].entry.edited = records[index].entry.proposed.map { $0 != records[index].entry.committed }
                dirty.insert(id)
            })
    }

    private func delete(_ record: ConversionLog.Record) {
        ConversionLog.shared.replace(record, with: nil)
        records.removeAll { $0.id == record.id }
        dirty.remove(record.id)
    }

    /// 編集した行をファイルへ書き戻す（1行ずつ。件数は多くないので十分）
    private func flush() {
        guard !dirty.isEmpty else { return }
        for record in records where dirty.contains(record.id) {
            let entry = record.entry
            guard !entry.reading.isEmpty, !entry.committed.isEmpty else { continue }
            ConversionLog.shared.replace(record, with: entry)
        }
        dirty = []
    }
}
