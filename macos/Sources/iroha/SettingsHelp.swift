import SwiftUI

/// 設定項目の説明を出す「?」ボタン。押すと説明を小さなウィンドウ（ポップオーバー）で出す。
///
/// 設定画面に説明文を並べると項目が見つけにくくなるので、細かな説明はここに入れる。
/// 状態・警告・エラーの表示はこれに入れず、画面にそのまま出す
struct HelpButton: View {
    private let text: String
    @State private var isShowing = false

    init(_ text: String) {
        self.text = text
    }

    var body: some View {
        Button {
            isShowing.toggle()
        } label: {
            Image(systemName: "questionmark.circle")
        }
        .buttonStyle(.borderless)
        .foregroundStyle(.secondary)
        .help("説明を表示")
        .accessibilityLabel("説明")
        .popover(isPresented: $isShowing, arrowEdge: .bottom) {
            Text(text)
                .font(.callout)
                .frame(width: 340, alignment: .leading)
                .fixedSize(horizontal: false, vertical: true)
                .padding(12)
                .textSelection(.enabled)
        }
    }
}

/// 項目名の右に「?」ボタンを置いたラベル。
///
/// `.disabled` をラベルごとかけると「?」も押せなくなるので、無効の見た目はここで付け、
/// 無効にするのは操作部品だけにする（機能がOFFでも説明は読める）
struct HelpLabel: View {
    let title: String
    let help: String
    var isDisabled = false
    /// 開発者モードのときだけ出す項目なら true（「開発者」の印を付ける。`DeveloperBadge`）
    var developer = false

    var body: some View {
        HStack(spacing: 4) {
            Text(title)
                .foregroundStyle(isDisabled ? .secondary : .primary)
            HelpButton(help)
            if developer { DeveloperBadge() }
        }
    }
}

/// 項目名の右に「?」ボタンを置いたトグル
struct HelpToggle: View {
    let title: String
    @Binding var isOn: Bool
    let help: String
    var isDisabled = false
    /// 開発者モードのときだけ出す項目なら true
    var developer = false

    var body: some View {
        LabeledContent {
            // LabeledContent の中ではフォームのスイッチ表示にならずチェックボックスになるので、
            // ほかのトグル（フォーム既定のスイッチ）と同じ見た目を指定する
            Toggle(title, isOn: $isOn)
                .labelsHidden()
                .toggleStyle(.switch)
                .controlSize(.mini)
                .disabled(isDisabled)
        } label: {
            HelpLabel(title: title, help: help, isDisabled: isDisabled, developer: developer)
        }
    }
}

/// 見出しの右に「?」ボタンを置いたセクション見出し（説明がセクション全体に関わるとき）。
/// `help` が nil なら「?」を出さない（開発者モードの印だけを付けたい見出し用）
struct HelpSectionHeader: View {
    let title: String
    var help: String?
    /// 節ごと開発者モードのときだけ出すなら true（節の中の項目には印を付けない）
    var developer = false

    var body: some View {
        HStack(spacing: 4) {
            Text(title)
            if let help { HelpButton(help) }
            if developer { DeveloperBadge() }
        }
    }
}

/// 開発者モードのときだけ表示する項目・節に付ける印（2026-11-07）。
///
/// 開発者モードを ON にすると一般ユーザ向けの項目と並んで出るので、どれが開発者モード専用かを見分けられるようにする。
/// 項目には `HelpLabel` / `HelpToggle` の `developer: true`、節ごとなら `HelpSectionHeader` の `developer: true` で付ける。
/// **`if developerMode` の中に項目を足したら、この印も付ける**
struct DeveloperBadge: View {
    var body: some View {
        Text("開発者")
            .font(.caption2.weight(.semibold))
            .foregroundStyle(Color.purple)
            .padding(.horizontal, 5)
            .padding(.vertical, 1)
            .background(Capsule().fill(Color.purple.opacity(0.15)))
            .fixedSize()
            .help("開発者モードのときだけ表示する項目です")
            .accessibilityLabel("開発者モードの項目")
    }
}
