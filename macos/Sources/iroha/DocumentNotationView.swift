import SwiftUI

/// 原稿から数えた表記の使い分けの一覧（設定 > 辞書・学習 > 自分の原稿に合わせる > 一覧...）。
/// 見るだけ。変えたいときは文書を入れ替える（`DocumentNotationStore` が読み直す）。
/// 原稿のフォルダが複数あるときは、上の選択で見るフォルダを替える（開いたときは使っているフォルダ）
struct DocumentNotationView: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject private var store = DocumentNotationStore.shared
    @State private var searchText = ""
    @State private var profile = DocumentNotationSettings.activeProfile
        ?? DocumentNotationStore.shared.snapshot.names.first ?? ""

    private struct Row: Identifiable {
        let id: String
        let reading: String
        let surfaces: String
    }

    private var rows: [Row] {
        let query = searchText.trimmingCharacters(in: .whitespaces)
        return (store.variantGroups[profile] ?? [])
            .filter { group in
                query.isEmpty || group.reading.contains(query) || group.surfaces.contains { $0.surface.contains(query) }
            }
            .map { group in
                Row(id: group.reading, reading: group.reading,
                    surfaces: group.surfaces.map { "\($0.surface)（\($0.count) 回）" }.joined(separator: "　"))
            }
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("原稿の表記の使い分け").font(.headline)
                if store.snapshot.names.count > 1 {
                    Picker("", selection: $profile) {
                        ForEach(store.snapshot.names, id: \.self) { name in
                            Text(DocumentNotationSettings.displayName(name)).tag(name)
                        }
                    }
                    .labelsHidden()
                    .fixedSize()
                }
                Spacer()
                TextField("検索", text: $searchText).textFieldStyle(.roundedBorder).frame(width: 200)
            }
            .padding(12)
            Divider()
            Table(rows) {
                TableColumn("読み") { Text($0.reading) }.width(min: 100, ideal: 140)
                TableColumn("使った表記と回数") { Text($0.surfaces) }
            }
            Divider()
            HStack {
                Text("回数の多い表記が選ばれやすくなります。変えたいときは、フォルダの文書を入れ替えてください。")
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("閉じる") { dismiss() }.keyboardShortcut(.defaultAction)
            }
            .padding(12)
        }
        .frame(width: 640, height: 460)
    }
}
