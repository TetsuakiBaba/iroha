import IrohaCore
import SwiftUI

/// ユーザ辞書の編集画面（設定ウィンドウからシートで開く）。
///
/// 編集内容は`UserDictionaryStore`（JSONファイル）に即時保存され、
/// 次の変換から反映される。
struct UserDictionaryView: View {
    @Environment(\.dismiss) private var dismiss

    @State private var entries: [UserDictionaryEntry] = UserDictionaryStore.shared.entries
    @State private var searchText = ""
    @State private var message: String?
    @State private var messageIsError = false
    @State private var importing = false
    @FocusState private var focusedField: UUID?

    private var filteredEntries: [UserDictionaryEntry] {
        let query = searchText.trimmingCharacters(in: .whitespaces)
        guard !query.isEmpty else { return entries }
        let reading = UserDictionary.normalizedReading(query)
        return entries.filter {
            $0.reading.contains(reading) || $0.word.contains(query)
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            entryList
            Divider()
            footer
        }
        .frame(width: 520, height: 460)
        .onChange(of: entries) { UserDictionaryStore.shared.replaceAll(entries) }
    }

    private var header: some View {
        HStack {
            Text("ユーザ辞書").font(.headline)
            Spacer()
            TextField("検索", text: $searchText)
                .textFieldStyle(.roundedBorder)
                .frame(width: 160)
        }
        .padding(12)
    }

    private var entryList: some View {
        Group {
            if entries.isEmpty {
                VStack(spacing: 8) {
                    Text("登録された単語はありません")
                        .foregroundStyle(.secondary)
                    Text("「＋」で追加するか、macOSのユーザ辞書から取り込めます。")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List {
                    HStack {
                        Text("よみ").frame(width: 160, alignment: .leading)
                        Text("単語").frame(maxWidth: .infinity, alignment: .leading)
                        Spacer().frame(width: 20)
                    }
                    .font(.caption)
                    .foregroundStyle(.secondary)

                    ForEach(filteredEntries) { entry in
                        row(for: entry)
                    }
                }
                .listStyle(.inset)
            }
        }
    }

    private func row(for entry: UserDictionaryEntry) -> some View {
        HStack(spacing: 8) {
            TextField("よみ", text: binding(for: entry.id, keyPath: \.reading))
                .frame(width: 160)
                .focused($focusedField, equals: entry.id)
            TextField("単語", text: binding(for: entry.id, keyPath: \.word))
                .frame(maxWidth: .infinity)
            Button {
                entries.removeAll { $0.id == entry.id }
            } label: {
                Image(systemName: "minus.circle")
            }
            .buttonStyle(.borderless)
            .help("この単語を削除")
        }
        .textFieldStyle(.plain)
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Button {
                    let entry = UserDictionaryEntry(reading: "", word: "")
                    // 並べ替え（読み順）で見失わないよう、保存前の一覧の先頭に置く
                    entries.insert(entry, at: 0)
                    focusedField = entry.id
                } label: {
                    Image(systemName: "plus")
                }
                .help("単語を追加")

                Button("macOSのユーザ辞書から取り込む") { importFromSystem() }
                    .disabled(importing)
                if importing {
                    ProgressView().controlSize(.small)
                }
                Spacer()
                Text("\(entries.count) 件").foregroundStyle(.secondary).font(.caption)
                Button("閉じる") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
            if let message {
                Text(message)
                    .font(.caption)
                    .foregroundStyle(messageIsError ? .red : .secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(12)
    }

    /// 一覧の中の1エントリの特定フィールドへのBinding
    private func binding(
        for id: UUID, keyPath: WritableKeyPath<UserDictionaryEntry, String>
    ) -> Binding<String> {
        Binding(
            get: { entries.first { $0.id == id }?[keyPath: keyPath] ?? "" },
            set: { newValue in
                guard let index = entries.firstIndex(where: { $0.id == id }) else { return }
                entries[index][keyPath: keyPath] = newValue
                // 手で直したエントリは以後の同期で上書き・削除されないようにする
                entries[index].source = .manual
            }
        )
    }

    private func importFromSystem() {
        importing = true
        message = nil
        Task {
            do {
                let result = try SystemUserDictionary.sync()
                await MainActor.run {
                    importing = false
                    entries = UserDictionaryStore.shared.entries
                    messageIsError = false
                    message = summary(of: result)
                }
            } catch {
                await MainActor.run {
                    importing = false
                    messageIsError = true
                    message = error.localizedDescription
                }
            }
        }
    }

    private func summary(of result: UserDictionaryStore.SyncResult) -> String {
        var parts = ["追加 \(result.added) 件", "変更なし \(result.unchanged) 件"]
        if result.removed > 0 {
            parts.append("macOS側で削除された \(result.removed) 件を削除")
        }
        if result.skipped > 0 {
            parts.append("ひらがな以外のよみ \(result.skipped) 件は対象外")
        }
        return parts.joined(separator: " / ")
    }
}
