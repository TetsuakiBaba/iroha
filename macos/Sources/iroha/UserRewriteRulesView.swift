import IrohaCore
import SwiftUI

/// ユーザ定義の変換ルール（User Rewriter）の編集画面（設定ウィンドウからシートで開く）。
///
/// 編集内容は`UserRewriteRuleStore`（JSONファイル）に即時保存され、次の変換から反映される。
/// 「プレビュー」列には出力テンプレートを今の日時で展開した結果を出す
/// （書き損じたプレースホルダはそのまま見えるので気づける）。
struct UserRewriteRulesView: View {
    @Environment(\.dismiss) private var dismiss

    @State private var rules: [UserRewriteRule] = UserRewriteRuleStore.shared.rules
    @State private var searchText = ""
    @FocusState private var focusedField: UUID?

    private var filteredRules: [UserRewriteRule] {
        let query = searchText.trimmingCharacters(in: .whitespaces)
        guard !query.isEmpty else { return rules }
        let trigger = UserRewriteRule.normalizedTrigger(query)
        return rules.filter {
            $0.trigger.contains(trigger) || $0.output.contains(query)
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            ruleList
            Divider()
            footer
        }
        .frame(width: 640, height: 480)
        .onChange(of: rules) { UserRewriteRuleStore.shared.replaceAll(rules) }
    }

    private var header: some View {
        HStack {
            Text("変換ルール").font(.headline)
            Spacer()
            TextField("検索", text: $searchText)
                .textFieldStyle(.roundedBorder)
                .frame(width: 160)
        }
        .padding(12)
    }

    private var ruleList: some View {
        Group {
            if rules.isEmpty {
                VStack(spacing: 8) {
                    Text("登録された変換ルールはありません")
                        .foregroundStyle(.secondary)
                    Text("「＋」で追加します。例: トリガー「きょう」→ 出力「{{date:yyyy/MM/dd}}」")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                // プレビューは時刻を含むので1分ごとに描き直す
                TimelineView(.everyMinute) { timeline in
                    List {
                        HStack(spacing: 8) {
                            Spacer().frame(width: 20)
                            Text("トリガー").frame(width: 120, alignment: .leading)
                            Text("出力").frame(maxWidth: .infinity, alignment: .leading)
                            Text("プレビュー").frame(width: 150, alignment: .leading)
                            Spacer().frame(width: 20)
                        }
                        .font(.caption)
                        .foregroundStyle(.secondary)

                        ForEach(filteredRules) { rule in
                            row(for: rule, now: timeline.date)
                        }
                    }
                    .listStyle(.inset)
                }
            }
        }
    }

    private func row(for rule: UserRewriteRule, now: Date) -> some View {
        let template = RewriteTemplate(rule.output)
        let preview = template.render(in: RewriteContext(now: now))
        let hasUnknown = !template.unknownPlaceholders.isEmpty
        return HStack(spacing: 8) {
            Toggle("", isOn: binding(for: rule.id, keyPath: \.isEnabled))
                .toggleStyle(.checkbox)
                .labelsHidden()
                .frame(width: 20)
                .help("このルールを有効にする")
            TextField("トリガー", text: binding(for: rule.id, keyPath: \.trigger))
                .frame(width: 120)
                .focused($focusedField, equals: rule.id)
            TextField("出力", text: binding(for: rule.id, keyPath: \.output))
                .frame(maxWidth: .infinity)
            Text(preview)
                .lineLimit(1)
                .truncationMode(.tail)
                .foregroundStyle(hasUnknown ? .red : .secondary)
                .frame(width: 150, alignment: .leading)
                .help(hasUnknown
                      ? "未対応のプレースホルダ: " + template.unknownPlaceholders.joined(separator: ", ")
                      : preview)
            Button {
                rules.removeAll { $0.id == rule.id }
            } label: {
                Image(systemName: "minus.circle")
            }
            .buttonStyle(.borderless)
            .help("このルールを削除")
        }
        .textFieldStyle(.plain)
        .opacity(rule.isEnabled ? 1 : 0.5)
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Button {
                    let rule = UserRewriteRule(trigger: "", output: "")
                    // 保存されるのは内容を入れてからなので、一覧の末尾に置いてフォーカスする
                    rules.append(rule)
                    focusedField = rule.id
                } label: {
                    Image(systemName: "plus")
                }
                .help("ルールを追加")
                Spacer()
                Text("\(rules.count) 件").foregroundStyle(.secondary).font(.caption)
                Button("閉じる") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
            Text("トリガー（よみ）が文節の読み全体と一致すると、出力を変換候補に加えます。"
                + "出力には通常の文字と次のプレースホルダを混ぜて書けます:")
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Text("{{date:yyyy/MM/dd}} 日付　{{time:HH:mm}} 時刻　{{datetime:yyyy/MM/dd HH:mm}} 日時　"
                + "{{wareki:Gy年M月d日}} 和暦（書式は省略可）\n"
                + "{{date+1}} 明日　{{date-1}} 昨日　{{wareki+2}} 明後日（+N/-N で日数をずらす）")
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(12)
    }

    /// 一覧の中の1ルールの特定フィールドへのBinding
    private func binding<Value>(
        for id: UUID, keyPath: WritableKeyPath<UserRewriteRule, Value>
    ) -> Binding<Value> where Value: Equatable {
        Binding(
            get: {
                rules.first { $0.id == id }?[keyPath: keyPath]
                    ?? UserRewriteRule(trigger: "", output: "")[keyPath: keyPath]
            },
            set: { newValue in
                guard let index = rules.firstIndex(where: { $0.id == id }) else { return }
                rules[index][keyPath: keyPath] = newValue
            }
        )
    }
}
