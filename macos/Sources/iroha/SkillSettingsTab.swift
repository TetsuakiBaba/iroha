import SwiftUI
import IrohaCore

/// 設定 > スキル（2026-11-11）。iroha の ID を見せ、青空写経のコードを受け取り、解除するスキルを選ばせる。
///
/// 画面に出す算数は `SkillState` と同じ: コード 1 つでスキル 1 つ、別々の作品のコードが 7 つ揃うとすべて。
/// 解除していないスキルの設定項目はほかのタブに出さないので、どんなスキルがあるかはこのタブの一覧で見せる
struct SkillSettingsTab: View {
    @ObservedObject private var skills = SkillObserver.shared
    @State private var codeText = ""
    @State private var message: Message?
    @State private var choosing: Skill?
    @State private var copied = false

    private struct Message {
        let text: String
        let isError: Bool
    }

    private static let shakyoURL = URL(string: "https://tetsuakibaba.github.io/AozoraShakyo/")!

    var body: some View {
        let state = skills.state
        Form {
            Section {
                LabeledContent("集めたコード") {
                    HStack(spacing: 6) {
                        ForEach(0..<SkillState.requiredForAll, id: \.self) { index in
                            Image(systemName: index < state.codeCount ? "circle.fill" : "circle")
                                .foregroundStyle(index < state.codeCount ? Color.accentColor : Color.secondary)
                                .font(.caption)
                        }
                        Text("\(min(state.codeCount, SkillState.requiredForAll)) / \(SkillState.requiredForAll)")
                            .monospacedDigit()
                            .foregroundStyle(.secondary)
                    }
                    .accessibilityElement(children: .ignore)
                    .accessibilityLabel("集めたコード \(state.codeCount) 個、\(SkillState.requiredForAll) 個で全部")
                }
                if state.isAllUnlocked {
                    Label("コードが \(SkillState.requiredForAll) つ揃いました。すべてのスキルを使えます。", systemImage: "checkmark.seal.fill")
                        .foregroundStyle(.green)
                }
                HStack {
                    Button("青空写経を開く") { NSWorkspace.shared.open(Self.shakyoURL) }
                    Spacer()
                }
            } header: {
                HelpSectionHeader(
                    title: "スキル",
                    help: "青空写経（青空文庫の作品を書き写す Web ページ）で 1 作品を書き上げると、コードが 1 つ出ます。"
                        + "コード 1 つで、下の一覧からスキルを 1 つ選んで解除できます。"
                        + "別々の作品のコードが \(SkillState.requiredForAll) つ揃うと、すべてのスキルを使えるようになります"
                        + "（同じ作品のコードは 1 つと数えます）。"
                        + "解除したスキルの設定項目は、それぞれのタブに出ます。"
                        + "解除の記録はデータフォルダの skills.json に置き、同じデータフォルダを使う Mac で共有します。")
            }

            Section {
                LabeledContent {
                    HStack {
                        Text(SkillCode.formatID(state.id ?? ""))
                            .font(.system(.body, design: .monospaced))
                            .textSelection(.enabled)
                        Button(copied ? "コピーしました" : "コピー") {
                            guard let id = state.id else { return }
                            NSPasteboard.general.clearContents()
                            NSPasteboard.general.setString(SkillCode.formatID(id), forType: .string)
                            copied = true
                        }
                        .disabled(state.id == nil)
                    }
                } label: {
                    HelpLabel(
                        title: "iroha の ID",
                        help: "青空写経の「納める」の画面に、この ID を入れてください。コードはこの ID に合わせて作られるので、"
                            + "ほかの人の ID で作ったコードは使えません。")
                }
                HStack {
                    TextField("", text: $codeText, prompt: Text("例: 03-K9T4WB"))
                        .labelsHidden()
                        .textFieldStyle(.roundedBorder)
                        .font(.system(.body, design: .monospaced))
                        .frame(width: 180)
                        .onSubmit(submitCode)
                    Button("コードを入れる", action: submitCode)
                        .disabled(codeText.trimmingCharacters(in: .whitespaces).isEmpty)
                    Spacer()
                }
                if let message {
                    Text(message.text)
                        .font(.callout)
                        .foregroundStyle(message.isError ? Color.red : Color.secondary)
                }
            } header: {
                Text("コード")
            }

            Section {
                ForEach(Skill.allCases) { skill in
                    SkillRow(
                        skill: skill, isUnlocked: skills.has(skill),
                        canChoose: state.unusedCodeCount > 0
                    ) { choosing = skill }
                }
            } header: {
                HStack(spacing: 4) {
                    Text("スキルの一覧")
                    if state.unusedCodeCount > 0 {
                        Text("（あと \(state.unusedCodeCount) つ選べます）").foregroundStyle(Color.accentColor)
                    }
                }
            } footer: {
                if state.isAllUnlocked {
                    Text("このほかに「アプリの文章を文脈に使う」（入力）と「記号・顔文字・絵文字を候補に出す」（辞書・学習）の切り替えも出ています。")
                } else {
                    Text("コードが \(SkillState.requiredForAll) つ揃うと、一覧のスキルがすべて解除されます。")
                }
            }
        }
        .formStyle(.grouped)
        // ID を作るのは、このタブを初めて開いたとき（`SkillStore.ensureID` のコメント参照）
        .onAppear { SkillStore.shared.ensureID() }
        .alert(
            "「\(choosing?.title ?? "")」を解除しますか？",
            isPresented: Binding(get: { choosing != nil }, set: { if !$0 { choosing = nil } }),
            presenting: choosing
        ) { skill in
            Button("解除する") {
                if SkillStore.shared.choose(skill: skill.rawValue) {
                    message = Message(text: "「\(skill.title)」を解除しました。設定の「\(skill.location)」に項目が出ています。", isError: false)
                }
            }
            Button("キャンセル", role: .cancel) {}
        } message: { _ in
            Text("コードを 1 つ使います。選んだスキルは、あとから替えられません。")
        }
    }

    private func submitCode() {
        let text = codeText.trimmingCharacters(in: .whitespaces)
        guard !text.isEmpty else { return }
        SkillStore.shared.ensureID()
        switch SkillStore.shared.addCode(text) {
        case .added(let work):
            codeText = ""
            let state = SkillStore.shared.state
            if state.isAllUnlocked {
                message = Message(
                    text: "作品 \(work) のコードを受け取りました。コードが \(SkillState.requiredForAll) つ揃ったので、すべてのスキルを解除しました。",
                    isError: false)
            } else {
                message = Message(text: "作品 \(work) のコードを受け取りました。下の一覧からスキルを 1 つ選んでください。", isError: false)
            }
        case .alreadyUsed(let work):
            message = Message(text: "作品 \(work) のコードは、もう入れてあります。別の作品のコードを入れてください。", isError: true)
        case .invalid, .noID:
            message = Message(
                text: "コードが違います。青空写経に入れた ID が上の ID と同じか、コードの文字（半角の英数字）を確かめてください。",
                isError: true)
        }
    }
}

/// スキルの一覧の 1 行: 名前と説明、右に状態（解除済み・解除するボタン・未解除・この Mac では使えない）
private struct SkillRow: View {
    let skill: Skill
    let isUnlocked: Bool
    /// 使っていないコードがある
    let canChoose: Bool
    let choose: () -> Void

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text(skill.title)
                Text(skill.summary)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if let reason = skill.unavailableReason {
                    Text(reason).font(.caption).foregroundStyle(.orange)
                }
            }
            Spacer()
            if isUnlocked {
                VStack(alignment: .trailing, spacing: 2) {
                    Label("解除済み", systemImage: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                    Text("設定 > \(skill.location)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            } else if canChoose, skill.unavailableReason == nil {
                Button("解除する", action: choose)
            } else {
                Label("未解除", systemImage: "lock.fill")
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 2)
    }
}
