import ApplicationServices
import SwiftUI
import IrohaCore
import IrohaMLX

/// 設定ウィンドウのタブ
enum SettingsTab: Hashable {
    case input       // 入力・変換のふるまい
    case dictionary  // ユーザ辞書・変換ルール・変換の学習・変換記録
    case selection   // 他アプリの選択テキストのAI編集 + 選択した文字数の表示
    case model       // かな漢字変換に使うモデルとAIサービス
    case about       // アップデートとバージョン情報
}

/// 設定ウィンドウの一時的な表示状態（メニューから直接ユーザ辞書を開く等）。
/// ウィンドウの生成タイミングに依存しないよう、プロセスで1つを共有する
@MainActor
final class SettingsUIState: ObservableObject {
    static let shared = SettingsUIState()
    @Published var selectedTab: SettingsTab = .input
    @Published var showingUserDictionary = false
    @Published var showingRewriteRules = false
    @Published var showingConversionLog = false
    @Published var showingLearning = false
    @Published var showingInputHistory = false

    /// メニューの「ユーザ辞書...」から呼ぶ: 辞書・学習タブを開いて編集シートを出す
    func openUserDictionary() {
        selectedTab = .dictionary
        showingUserDictionary = true
    }

    /// メニューの「変換ルール...」から呼ぶ: 辞書・学習タブを開いてルール編集シートを出す
    func openRewriteRules() {
        selectedTab = .dictionary
        showingRewriteRules = true
    }
}

/// irohaの設定ウィンドウ。値はUserDefaults（irohaのドメイン）に保存され、
/// コントローラ側が都度読み出す。
struct SettingsView: View {
    @ObservedObject private var uiState = SettingsUIState.shared

    var body: some View {
        TabView(selection: $uiState.selectedTab) {
            InputSettingsTab()
                .tabItem { Label("入力", systemImage: "keyboard") }
                .tag(SettingsTab.input)
            DictionarySettingsTab()
                .tabItem { Label("辞書・学習", systemImage: "character.book.closed") }
                .tag(SettingsTab.dictionary)
            SelectionSettingsTab()
                .tabItem { Label("選択テキスト", systemImage: "cursorarrow.rays") }
                .tag(SettingsTab.selection)
            ModelSettingsTab()
                .tabItem { Label("モデル", systemImage: "cube") }
                .tag(SettingsTab.model)
            AboutSettingsTab()
                .tabItem { Label("情報", systemImage: "info.circle") }
                .tag(SettingsTab.about)
        }
        // タブごとに高さが変わらないよう固定サイズにする（収まらない分はフォーム内でスクロール）。
        // macOS 26ではタブがタイトルバーに入るため、全項目が折り畳まれない幅が要る
        .frame(minWidth: 660, idealWidth: 660, minHeight: 690, idealHeight: 690)
        .sheet(isPresented: $uiState.showingUserDictionary) { UserDictionaryView() }
        .sheet(isPresented: $uiState.showingRewriteRules) { UserRewriteRulesView() }
        .sheet(isPresented: $uiState.showingConversionLog) { ConversionLogView() }
        .sheet(isPresented: $uiState.showingLearning) { LearningView() }
        .sheet(isPresented: $uiState.showingInputHistory) { InputHistoryView() }
    }
}

// MARK: - 入力

private struct InputSettingsTab: View {
    @AppStorage("liveConversion") private var liveConversion = true
    @AppStorage(DocumentContextSettings.enabledKey) private var documentContext = true
    @AppStorage("candidateCount") private var candidateCount = 8
    @AppStorage("punctuationStyle") private var punctuationStyle = "、。"
    @AppStorage(PredictionSettings.predictiveEnabledKey) private var predictiveConversion = false
    @AppStorage(TypoNormalizerSettings.enabledKey) private var typoNormalizer = false
    @AppStorage(TypoNormalizerSettings.thresholdKey)
    private var typoThreshold = TypoNormalizer.defaultThreshold
    @AppStorage(TypoNormalizerSettings.delayMillisecondsKey)
    private var typoDelayMs = TypoNormalizerSettings.defaultDelayMilliseconds
    @AppStorage(TypoNormalizerSettings.minimumLengthKey)
    private var typoMinLength = TypoNormalizerSettings.defaultMinimumLength

    var body: some View {
        Form {
            Section("変換") {
                Toggle("ライブ変換", isOn: $liveConversion)
                Stepper(value: $candidateCount, in: 3...16) {
                    HStack {
                        HelpLabel(
                            title: "候補ウィンドウでモデルが並べる候補数",
                            help: "この数の下に、読みが一致する辞書の残りの候補（単漢字・異体字・人名など）が続きます。")
                        Spacer()
                        Text("\(candidateCount)").foregroundStyle(.secondary)
                    }
                }
                HelpToggle(
                    title: "アプリの文章を文脈に使う", isOn: $documentContext,
                    help: "入力を始めた位置の手前にある文章（最大40文字）をアプリから読み取り、変換の文脈にします。"
                        + "文章の途中に書き足すときや、別のアプリに移った直後でも前後に合った変換になります。"
                        + "文章を返さないアプリでは、irohaで直前に確定した文字列を文脈にします。")
            }

            Section("打ち間違いの訂正") {
                HelpToggle(
                    title: "打ち間違いを自動で直す", isOn: $typoNormalizer,
                    help: "入力の手が止まったとき、読みの打ち間違い（隣のキー・抜け・重複・入れ替え・"
                        + "「っ」の過不足）を直してから変換します。直したときは何をどう直したかを"
                        + "カーソルの下に表示し、そのままBackspaceを押すと打ったとおりの読みに戻せます。"
                        + "休止を待たずにスペースを押したときは、"
                        + "読みは変えずに候補ウィンドウに訂正を足します。")
                // 訂正モデルはアプリに同梱していない。ONにした時点で取得する
                TypoNormalizerModelRow(isEnabled: typoNormalizer)
                TypoDelayRow(milliseconds: $typoDelayMs, isDisabled: !typoNormalizer)
                LabeledContent {
                    HStack {
                        Text("\(typoMinLength)文字").foregroundStyle(.secondary).monospacedDigit()
                        Stepper("訂正する読みの最低文字数", value: $typoMinLength,
                                in: TypoNormalizerSettings.minimumLengthRange)
                            .labelsHidden()
                            .disabled(!typoNormalizer)
                    }
                } label: {
                    HelpLabel(
                        title: "訂正する読みの最低文字数",
                        help: "読みがこの文字数に満たないときは直しません。短い読みは正しく打っていても"
                            + "別の語の打ち間違いに見えやすいためです（例:「さど」が「さいど」に直る）。",
                        isDisabled: !typoNormalizer)
                }
                TypoThresholdRow(threshold: $typoThreshold, isDisabled: !typoNormalizer)
            }

            Section("予測変換") {
                HelpToggle(
                    title: "入力履歴から予測する", isOn: $predictiveConversion,
                    help: "入力中、読みの先頭が一致する語を、これまでに確定した語（入力履歴）から"
                        + "最大3件カーソルの下に表示します。読みを2文字打つと出ます。"
                        + "Tabで選び（押すたびに次の候補、Shift+Tabで前の候補）、選んだまま続きを打つと"
                        + "その候補が入ります。Returnで入れて確定、Escで選ぶのをやめます。クリックでも入ります。"
                        + "入れた部分はBackspaceで打った読みに戻せます。"
                        + "入力履歴はこの設定がONの間だけ、ローマ字で入力して確定した語を記録します"
                        + "（Shift+英字で入れた英字やF9・F10で英数にした部分は記録しません）。"
                        + "記録はデータフォルダの input-history/ にMacごとのファイルとして残り、"
                        + "同じデータフォルダを使うMacの間で合算されます。")
                InputHistoryRow(isEnabled: predictiveConversion)
            }

            Section("句読点") {
                Picker(selection: $punctuationStyle) {
                    Text("、。").tag("、。")
                    Text("，．").tag("，．")
                } label: {
                    HelpLabel(
                        title: "句読点スタイル",
                        help: "変更は次の入力から反映されます。入力中は ⌃.（control + ピリオド）でも切り替えられます。")
                }
                .pickerStyle(.segmented)
            }

            // 未確定文字列をAIに渡して確定する（使うAIサービスは「モデル」タブで選ぶ）
            Section {
                AICommitPresetEditor(index: 0)
                AICommitPresetEditor(index: 1)
                AICommitPresetEditor(index: 2)
            } header: {
                HelpSectionHeader(
                    title: "AI変換",
                    help: "修飾キー+Returnで、入力中の未確定文字列をAIに渡します。"
                        + "結果が日本語なら文節に区切った未確定文字列で返し、もう一度Returnで確定します。"
                        + "英訳など日本語を含まない結果はそのまま確定します。"
                        + "ショートカットは「Option+Return」「Shift+Cmd+J」のように修飾キー（Cmd・Ctrl・Option・Shift）と"
                        + "キーを + でつないで書きます。入力中（未確定文字列があるとき）だけ効きます。"
                        + "⌃Returnは、テキストエディットなど多くのアプリでmacOSが右クリックメニューに使うため効きません。"
                        + "使うAIサービスは「モデル」タブで選びます。")
            }
        }
        .formStyle(.grouped)
    }
}

/// 入力履歴の件数と、確認・編集・変換記録からの取り込み
private struct InputHistoryRow: View {
    let isEnabled: Bool
    @ObservedObject private var uiState = SettingsUIState.shared
    @State private var count = InputHistoryStore.shared.count
    @State private var importMessage: String?

    var body: some View {
        LabeledContent {
            HStack {
                Text("\(count) 語").foregroundStyle(.secondary)
                Button("確認・編集...") { uiState.showingInputHistory = true }
                    .disabled(count == 0)
                Button("変換記録から取り込む") {
                    let imported = InputHistoryStore.shared.importConversionLog()
                    importMessage = imported == 0 ? "新しい記録はありません" : "\(imported) 件の確定を取り込みました"
                }
                .disabled(!isEnabled)
            }
        } label: {
            VStack(alignment: .leading, spacing: 2) {
                HelpLabel(
                    title: "入力履歴",
                    help: "「変換記録から取り込む」は、辞書・学習タブの「変換記録」に残っている"
                        + "このMacの確定を入力履歴に足します。前回取り込んだ後の記録だけを足すので、"
                        + "何度押しても二重には数えません。",
                    isDisabled: !isEnabled)
                if let importMessage {
                    Text(importMessage).font(.caption).foregroundStyle(.secondary)
                }
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: InputHistoryStore.didChangeNotification)) { _ in
            count = InputHistoryStore.shared.count
        }
    }
}

/// 訂正モデルの取得状況。アプリに同梱していないので、ONにした時点でここから落としてくる。
///
/// 重みは本体コード(MIT)と別ライセンス（CC BY-SA 4.0）なので、配布物を分けてある。
/// 取得したものは `<データフォルダ>/models/typo-normalizer/` に入り、保存場所を共有フォルダに
/// している人は1回落とせば全部のMacで使える
private struct TypoNormalizerModelRow: View {
    let isEnabled: Bool
    @ObservedObject private var downloader = TypoNormalizerDownloader.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                HelpLabel(title: "訂正モデル", help: downloader.offerDescription)
                Spacer()
                content
            }
            if case .downloading(let progress) = downloader.state {
                ProgressView(value: progress)
            }
            if case .failed(let message) = downloader.state {
                Text(message)
                    .font(.caption)
                    .foregroundStyle(.red)
            }
            if let notice = downloader.outdatedNotice, !downloader.isBusy {
                Text(notice)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .onAppear { downloader.refreshCatalogIfNeeded() }
        // トグルをONにした時点で取りにいく（OFFのまま勝手に通信しない）
        .onChange(of: isEnabled) { _, enabled in
            if enabled, downloader.installed == nil { downloader.install() }
        }
    }

    @ViewBuilder
    private var content: some View {
        switch downloader.state {
        case .loadingCatalog:
            HStack(spacing: 6) {
                ProgressView().controlSize(.small)
                Text("確認中…").foregroundStyle(.secondary)
            }
        case .downloading(let progress):
            HStack(spacing: 6) {
                Text("ダウンロード中 \(Int(progress * 100))%").foregroundStyle(.secondary)
                Button("中止") { downloader.cancel() }
            }
        case .verifying:
            Text("検証中…").foregroundStyle(.secondary)
        case .idle, .failed:
            if let installed = downloader.installed {
                HStack(spacing: 6) {
                    // 名前（「標準」）だけでは版が分からないので ID（small-v3 など）を添える
                    Text("\(installed.name)（\(installed.id)）").foregroundStyle(.secondary)
                    Button("削除") { downloader.remove() }
                }
            } else {
                Button("ダウンロード") { downloader.install() }
            }
        }
    }
}

/// 打ち間違いの訂正を走らせるまでの休止時間（ミリ秒）
private struct TypoDelayRow: View {
    @Binding var milliseconds: Int
    var isDisabled = false
    private static let step = 50.0
    private static let range = Double(TypoNormalizerSettings.delayMillisecondsRange.lowerBound)
        ... Double(TypoNormalizerSettings.delayMillisecondsRange.upperBound)

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                HelpLabel(
                    title: "訂正するまでの休止時間",
                    help: "キーを離してからこの時間だけ何も押さなければ訂正します。"
                        + "短いほど早く直りますが、語の途中で考えているだけのときにも動きます。",
                    isDisabled: isDisabled)
                Spacer()
                Text("\(milliseconds) ms")
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
            }
            Slider(value: sliderValue, in: Self.range, step: Self.step)
                .disabled(isDisabled)
        }
    }

    private var sliderValue: Binding<Double> {
        Binding(
            get: { Double(milliseconds) },
            set: { milliseconds = Int(($0 / Self.step).rounded() * Self.step) }
        )
    }
}

/// 打ち間違い訂正を採用する margin のしきい値。既定 2.0（SWIFT-PORT.md §4 の推奨）。
///
/// 数字そのものはユーザに意味が伝わらないので、目安の言葉を添える。
/// 訂正率・過剰訂正率の実測値は合成した打ち間違いの分布で測ったもので、実際の打ち間違いの
/// 分布ではないため、UIでは割合を約束しない（SWIFT-PORT.md §1）
private struct TypoThresholdRow: View {
    @Binding var threshold: Double
    var isDisabled = false

    /// しきい値の目安。境目は test 10,000 件で測った曲線の形に合わせてある
    /// （2.0 付近から過剰訂正が 1% を切り、5.0 を超えると訂正がほとんど出なくなる）
    private var label: String {
        switch threshold {
        case ..<1.0: return "よく拾う"
        case ..<3.0: return threshold == TypoNormalizer.defaultThreshold ? "標準・既定" : "標準"
        case ..<5.0: return "慎重"
        default: return "ほとんど出さない"
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                HelpLabel(
                    title: "訂正を出す確信の強さ",
                    help: "大きくするほど、モデルがよほど確信したときしか直しません。"
                        + "小さくすると打ち間違いをよく拾いますが、正しく打った読みも直してしまいます。",
                    isDisabled: isDisabled)
                Spacer()
                Text(String(format: "%.1f", threshold))
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
                Text("（\(label)）")
                    .foregroundStyle(.secondary)
            }
            HStack(spacing: 8) {
                Slider(value: $threshold, in: 0...8, step: 0.5) {
                    EmptyView()
                } minimumValueLabel: {
                    Text("よく拾う").font(.caption).foregroundStyle(.secondary)
                } maximumValueLabel: {
                    Text("慎重").font(.caption).foregroundStyle(.secondary)
                }
                Button("既定") { threshold = TypoNormalizer.defaultThreshold }
                    .disabled(threshold == TypoNormalizer.defaultThreshold)
            }
            .disabled(isDisabled)
        }
    }
}

// MARK: - 辞書・学習

private struct DictionarySettingsTab: View {
    @AppStorage(LearningSettings.enabledKey) private var learningEnabled = true
    @AppStorage(ConversionLogSettings.enabledKey) private var conversionLogEnabled = false
    @AppStorage(ConversionLogSettings.scopeKey) private var conversionLogScope = ConversionLogSettings.Scope.all.rawValue
    @AppStorage(UserDictionarySync.autoSyncKey) private var syncSystemDictionary = false
    @ObservedObject private var uiState = SettingsUIState.shared

    @State private var userDictionaryCount = UserDictionaryStore.shared.entries.count
    @State private var rewriteRuleCount = UserRewriteRuleStore.shared.rules.count
    @State private var learningCount = LearningStore.shared.count
    @State private var conversionLogSize = ConversionLog.shared.totalSize()
    @State private var conversionLogCount = ConversionLog.shared.entryCount()
    @State private var showingConversionLogDeleteConfirmation = false

    var body: some View {
        Form {
            Section("ユーザ辞書") {
                LabeledContent("登録単語") {
                    HStack {
                        Text("\(userDictionaryCount) 件").foregroundStyle(.secondary)
                        Button("編集...") { uiState.showingUserDictionary = true }
                    }
                }
                HelpToggle(
                    title: "起動時にmacOSのユーザ辞書を取り込む", isOn: $syncSystemDictionary,
                    help: "「システム設定 > キーボード > ユーザ辞書」に登録した単語を取り込みます"
                        + "（読み取りのみ。macOS側の辞書は変更しません）。"
                        + "取り込んだ単語をirohaで編集すると、以後の取り込みでは上書きされません。")
            }

            Section {
                LabeledContent("登録ルール") {
                    HStack {
                        Text("\(rewriteRuleCount) 件").foregroundStyle(.secondary)
                        Button("編集...") { uiState.showingRewriteRules = true }
                    }
                }
            } header: {
                HelpSectionHeader(
                    title: "変換ルール",
                    help: "トリガー（よみ）と出力の組を登録すると、文節の読みがトリガーに一致したとき"
                        + "出力を変換候補に加えます。出力には {{date:yyyy/MM/dd}} や {{time:HH:mm}} の"
                        + "ようなプレースホルダを書けて、変換のたびに今の日付・時刻に置き換わります"
                        + "（例:「きょう」→ 2026/09/06）。")
            }

            Section("変換の学習") {
                HelpToggle(
                    title: "変換の修正を学習する", isOn: $learningEnabled,
                    help: "文節変換（スペースキー）で候補を選び直して確定すると、入力した読みの全体・"
                        + "確定した文字列・直す前の変換結果を覚えます。次に同じ読みを入力して、変換結果が"
                        + "直す前と同じになったときに、覚えた文字列に差し替えます"
                        + "（「きしゃ」を「貴社」に直すと、次から「きしゃ」はそう変換されます）。"
                        + "前の文章によって変換結果が変わったときは差し替えず、候補ウィンドウに出します。"
                        + "読みの一部には当てはめません。")
                LabeledContent("学習した変換") {
                    HStack {
                        Text("\(learningCount) 件").foregroundStyle(.secondary)
                        Button("編集...") { uiState.showingLearning = true }
                        Button("Finderで表示") {
                            // 学習ファイル（learning.json）をFinderで選択状態にして見せる。
                            // まだ1件も学習していなくてファイルが無いときはフォルダを開く
                            let url = LearningStore.defaultURL
                            if FileManager.default.fileExists(atPath: url.path) {
                                NSWorkspace.shared.activateFileViewerSelecting([url])
                            } else {
                                let dir = url.deletingLastPathComponent()
                                try? FileManager.default.createDirectory(
                                    at: dir, withIntermediateDirectories: true)
                                NSWorkspace.shared.open(dir)
                            }
                        }
                        Button("リセット") { LearningStore.shared.reset() }
                            .disabled(learningCount == 0)
                    }
                }
            }

            Section("変換記録") {
                HelpToggle(
                    title: "確定した変換を記録する", isOn: $conversionLogEnabled,
                    help: "確定した変換を、そのときモデルに渡した文脈（カーソル手前の文章の末尾40文字）・読み・"
                        + "モデルの出力・確定した文字列とともに1件ずつ記録します。"
                        + "追加学習は「この文脈でこの読みならこう変換する」を学ぶので、"
                        + "左文脈のない確定（起動直後やフォーカス移動直後の1語目）は記録しません。"
                        + "「確認・編集...」で中身を見て、打ち間違いをそのまま確定した行は直すか削除できます。"
                        + "記録はデータフォルダ内の logs/conversions/ にこのMacのファイルとして残るだけで、"
                        + "どこにも送信されません。あとでこの記録を使って、自分の入力に合わせた変換モデルの"
                        + "追加学習（LoRAなど）ができます。上の「変換の学習」とは別のもので、"
                        + "記録しても変換の動作は変わりません。書いていた文章の一部がそのまま残るため、"
                        + "この設定は他のMacには同期されません。")
                Picker("記録する範囲", selection: $conversionLogScope) {
                    ForEach(ConversionLogSettings.Scope.allCases) { scope in
                        Text(scope.label).tag(scope.rawValue)
                    }
                }
                .disabled(!conversionLogEnabled)
                LabeledContent("記録したデータ") {
                    HStack {
                        Text("\(conversionLogCount) 件（\(Self.formatSize(conversionLogSize))）")
                            .foregroundStyle(.secondary)
                        Button("確認・編集...") { uiState.showingConversionLog = true }
                            .disabled(conversionLogCount == 0)
                        Button("削除...") { showingConversionLogDeleteConfirmation = true }
                            .disabled(conversionLogSize == 0)
                    }
                }
            }
        }
        .formStyle(.grouped)
        .confirmationDialog(
            "記録した変換をすべて削除しますか？", isPresented: $showingConversionLogDeleteConfirmation,
            titleVisibility: .visible
        ) {
            Button("削除", role: .destructive) { ConversionLog.shared.removeAll() }
        } message: {
            Text("logs/conversions/ 内のファイルを削除します。この操作は取り消せません。")
        }
        .onReceive(
            NotificationCenter.default.publisher(for: ConversionLog.didChangeNotification)
        ) { _ in
            conversionLogSize = ConversionLog.shared.totalSize()
            conversionLogCount = ConversionLog.shared.entryCount()
        }
        .onReceive(
            NotificationCenter.default.publisher(for: UserDictionaryStore.didChangeNotification)
        ) { _ in
            userDictionaryCount = UserDictionaryStore.shared.entries.count
        }
        .onReceive(
            NotificationCenter.default.publisher(for: UserRewriteRuleStore.didChangeNotification)
        ) { _ in
            rewriteRuleCount = UserRewriteRuleStore.shared.rules.count
        }
        .onReceive(
            NotificationCenter.default.publisher(for: LearningStore.didChangeNotification)
        ) { _ in
            learningCount = LearningStore.shared.count
        }
    }

    /// 変換記録の合計サイズの表示（0なら「なし」）
    private static func formatSize(_ bytes: Int64) -> String {
        guard bytes > 0 else { return "なし" }
        let formatter = ByteCountFormatter()
        formatter.countStyle = .file
        return formatter.string(fromByteCount: bytes)
    }
}

// MARK: - 選択テキスト（他アプリの選択テキストのAI編集 + 選択した文字数の表示）

private struct SelectionSettingsTab: View {
    @AppStorage(SelectionSettings.enabledKey) private var selectionEnabled = false
    @AppStorage(SelectionSettings.triggerModeKey) private var triggerMode = "bubble"
    @AppStorage(SelectionSettings.onDemandHotkeyKey) private var onDemandHotkey = "Ctrl+0"
    @AppStorage(SelectionSettings.excludedBundleIdsKey) private var excludedBundleIds = ""
    @AppStorage(SelectionSettings.characterCountKey) private var characterCount = false

    var body: some View {
        Form {
            Section {
                SelectionIntroRows(selectionEnabled: $selectionEnabled)
            } header: {
                HelpSectionHeader(
                    title: "選択テキストのAI編集",
                    help: "処理に使うAIサービス（Apple Intelligence・Ollamaなど）は「モデル」タブで設定します。")
            }

            Section("マウスで選択したとき") {
                Picker("トリガー", selection: $triggerMode) {
                    ForEach(SelectionTriggerMode.allCases) { mode in
                        Text(mode.label).tag(mode.rawValue)
                    }
                }
                .disabled(!selectionEnabled)
            }

            Section {
                HotkeyField(label: "ショートカット", hotkey: $onDemandHotkey)
                    .disabled(!selectionEnabled)
            } header: {
                HelpSectionHeader(
                    title: "その場でAIに指示",
                    help: "選択テキストに自由な指示を出せます（選択なしで押すとテキスト生成になります）。")
            }

            Section {
                SelectionPresetEditor(index: 0)
                SelectionPresetEditor(index: 1)
                SelectionPresetEditor(index: 2)
                SelectionPresetEditor(index: 3)
                SelectionPresetEditor(index: 4)
            } header: {
                HelpSectionHeader(
                    title: "プリセット",
                    help: "プリセットのショートカットは、テキストを選択していないときに押すと"
                        + "「テキストを生成」の入力欄になり、結果をカーソル位置へ挿入します。")
            }

            Section {
                TextField(
                    "", text: $excludedBundleIds,
                    prompt: Text("com.example.app, com.example.other"))
                    .textFieldStyle(.roundedBorder)
                    .disabled(!selectionEnabled)
            } header: {
                HelpSectionHeader(
                    title: "除外するアプリ",
                    help: "ここに書いたバンドルIDのアプリでは、マウス選択のトリガーを出しません"
                        + "（カンマまたは改行区切り）。")
            }

            // AI編集とは独立した機能（マウスで選択した文字数を選択範囲の近くに出す）。
            // アクセシビリティ権限と除外するアプリの設定はAI編集と共通なのでこのタブに置く
            Section("選択した文字数の表示") {
                HelpToggle(
                    title: "選択した文字数を表示する", isOn: $characterCount,
                    help: "マウスで選択（ドラッグ・ダブルクリック）すると、選択範囲の近くに文字数を数秒表示します"
                        + "（改行は数えず、空白があれば空白を除いた数も併記）。"
                        + "AI編集がONのときはアイコンに添えて表示します。"
                        + "アクセシビリティ権限が必要で、除外するアプリの設定も共通です。"
                        + "キーボードでの選択（Shift+矢印・⌘A）には反応しません。")
            }
        }
        .formStyle(.grouped)
    }
}

/// カード内の小見出し行（グループ内の区分け用）
private struct FormSubheader: View {
    private let text: String

    init(_ text: String) {
        self.text = text
    }

    var body: some View {
        Text(text)
            .font(.subheadline.weight(.semibold))
            .padding(.top, 4)
    }
}

/// 「選択テキストのAI編集」グループの導入行（有効化トグルと権限の状態）
private struct SelectionIntroRows: View {
    @Binding var selectionEnabled: Bool
    @State private var accessibilityGranted = AXIsProcessTrusted()

    var body: some View {
        HelpToggle(
            title: "選択テキストのAI編集を有効にする", isOn: $selectionEnabled,
            help: "どのアプリでも、選択したテキストをショートカットやマウス操作からAIで"
                + "処理して置き換えられます。"
                + "ショートカットはirohaが起動している間だけ有効です"
                + "（ログイン後に一度日本語入力すると起動します）。")
            .onAppear { accessibilityGranted = AXIsProcessTrusted() }
        LabeledContent {
            HStack {
                if accessibilityGranted {
                    Label("許可済み", systemImage: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                } else {
                    Label("未許可", systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                    Button("システム設定を開く") {
                        if let url = URL(
                            string: "x-apple.systempreferences:"
                                + "com.apple.preference.security?Privacy_Accessibility") {
                            NSWorkspace.shared.open(url)
                        }
                    }
                }
                Button("再確認") { accessibilityGranted = AXIsProcessTrusted() }
            }
        } label: {
            HelpLabel(title: "アクセシビリティ権限", help: "選択テキストの取得と置換にアクセシビリティ権限が必要です。")
        }
    }
}

/// AIサービス（バックエンド）の設定セクション。「モデル」タブに置き、
/// 入力タブの「AI変換」と選択テキストのAI編集が共通で使う
private struct AIServiceSection: View {
    @AppStorage(TranslationBackend.userDefaultsKey) private var translationService = "apple"
    @AppStorage("ollamaModel") private var ollamaModel = ""
    @AppStorage("lmStudioModel") private var lmStudioModel = ""
    @AppStorage("openAIModel") private var openAIModel = ""
    @AppStorage("openAIEndpoint") private var openAIEndpoint = ""
    @State private var openAIKey = RemoteTranslator.openAIAPIKey

    @State private var remoteModels: [String] = []
    @State private var remoteModelsError: String?
    @State private var loadingRemoteModels = false

    var body: some View {
        Section("AIサービス") {
            Picker(selection: $translationService) {
                Text("Apple Intelligence（オンデバイス）").tag("apple")
                Text("Ollama").tag("ollama")
                Text("LM Studio").tag("lmstudio")
                Text("OpenAI互換（外部API）").tag("openai")
            } label: {
                HelpLabel(title: "サービス", help: translationCaption)
            }
            if translationService == "openai" {
                VStack(alignment: .leading, spacing: 4) {
                    Text("エンドポイント")
                    TextField(
                        "", text: $openAIEndpoint,
                        prompt: Text("https://api.openai.com/v1"))
                        .textFieldStyle(.roundedBorder)
                }
                HStack {
                    Text("APIキー")
                    SecureField("", text: $openAIKey)
                        .textFieldStyle(.roundedBorder)
                        .onChange(of: openAIKey) {
                            SecretStore.set(openAIKey, for: RemoteTranslator.openAIKeyAccount)
                        }
                }
            }
            if translationService == "openai", remoteModels.isEmpty {
                // 一覧が取れないサービスもあるので手入力を許す
                HStack {
                    Text("モデル")
                    TextField("", text: $openAIModel, prompt: Text("gpt-4o-mini"))
                        .textFieldStyle(.roundedBorder)
                }
            } else if translationService != "apple" {
                Picker("モデル", selection: remoteModelBinding) {
                    Text("選択してください").tag("")
                    ForEach(remoteModelChoices, id: \.self) { model in
                        Text(model).tag(model)
                    }
                }
            }
            if translationService != "apple" {
                HStack {
                    Button("モデル一覧を更新") { fetchRemoteModels() }
                    if loadingRemoteModels {
                        ProgressView().controlSize(.small)
                    }
                }
                if let error = remoteModelsError {
                    Text(error).font(.caption).foregroundStyle(.red)
                }
            }
            // 外部への送信は説明に隠さず常に見せる
            if translationService == "openai" {
                Text("未確定文字列や選択テキストが外部サービスへ送信されます。")
                    .font(.caption)
                    .foregroundStyle(.orange)
            }
        }
        .onChange(of: translationService) { fetchRemoteModels() }
        .onAppear { fetchRemoteModels() }
    }

    private var remoteModelBinding: Binding<String> {
        switch translationService {
        case "ollama": return $ollamaModel
        case "openai": return $openAIModel
        default: return $lmStudioModel
        }
    }

    /// 選択中のモデルが一覧に無い場合（サーバ側で削除された等）も選択肢として残す
    private var remoteModelChoices: [String] {
        let current = remoteModelBinding.wrappedValue
        if !current.isEmpty, !remoteModels.contains(current) {
            return [current] + remoteModels
        }
        return remoteModels
    }

    private var translationCaption: String {
        let common = "「AI変換」と「選択テキストのAI編集」はここで選んだサービスを使います。"
        switch translationService {
        case "openai":
            return common + "OpenAI互換API（\(RemoteTranslator.openAIEndpoint)）に接続します。"
                + "未確定文字列や選択テキストが外部サービスへ送信されるので注意してください。"
                + "APIキーはKeychainに保存されます。失敗時は通常の確定になります。"
        case "ollama":
            return common + "ローカルのOllama（\(RemoteTranslator.ollamaEndpoint)）に接続します。"
                + "thinking（思考過程）は無効化されます。失敗時は通常の確定になります。"
        case "lmstudio":
            return common + "ローカルのLM Studio（\(RemoteTranslator.lmStudioEndpoint)）に接続します。"
                + "thinking（思考過程）は無効化されます。失敗時は通常の確定になります。"
        default:
            return common + (TranslationService.appleAvailable
                ? "オンデバイスAI（Apple Intelligence）で処理します。"
                : "Apple Intelligence（macOS 26以降で有効化）が必要です。"
                    + "利用できない間は通常の確定になります。")
        }
    }

    private func fetchRemoteModels() {
        guard translationService != "apple" else { return }
        let service: RemoteTranslator.Service
        switch translationService {
        case "ollama": service = .ollama
        case "openai": service = .openai
        default: service = .lmstudio
        }
        loadingRemoteModels = true
        remoteModelsError = nil
        Task {
            do {
                let models = try await RemoteTranslator.listModels(service: service)
                await MainActor.run {
                    loadingRemoteModels = false
                    remoteModels = models
                    if models.isEmpty {
                        remoteModelsError = "\(service.displayName)にモデルが見つかりません。"
                    } else if remoteModelBinding.wrappedValue.isEmpty {
                        // 未選択なら先頭を自動選択
                        remoteModelBinding.wrappedValue = models[0]
                    }
                }
            } catch {
                await MainActor.run {
                    loadingRemoteModels = false
                    remoteModels = []
                    remoteModelsError =
                        "\(service.displayName)に接続できません。起動しているか確認してください。"
                }
            }
        }
    }
}

/// AI変換プリセット1つ分の編集欄（名前・ショートカット・プロンプト）。
/// 3つ並べても縦に収まるよう、プロンプトは折りたたんでおく
private struct AICommitPresetEditor: View {
    private let index: Int
    @AppStorage private var name: String
    @AppStorage private var prompt: String
    @AppStorage private var hotkey: String
    @State private var expanded = false

    init(index: Int) {
        self.index = index
        let defaults = AICommitSettings.defaults[index]
        _name = AppStorage(wrappedValue: defaults.name, AICommitSettings.nameKey(index))
        _prompt = AppStorage(wrappedValue: defaults.prompt, AICommitSettings.promptKey(index))
        _hotkey = AppStorage(wrappedValue: defaults.shortcut, AICommitSettings.hotkeyKey(index))
    }

    var body: some View {
        FormSubheader("\(index + 1). \(headerName)")
            HStack {
                // ラベルを別に置く（TextFieldのタイトルにすると値が右寄せになる）
                Text("名前")
                TextField("", text: $name)
                    .labelsHidden()
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 140)
                Spacer()
            }
            AICommitShortcutField(index: index, hotkey: $hotkey)

            DisclosureGroup(isExpanded: $expanded) {
                TextEditor(text: $prompt)
                    .font(.body)
                    .frame(height: 72)
                    .scrollContentBackground(.hidden)
                    .padding(4)
                    .overlay(
                        RoundedRectangle(cornerRadius: 6)
                            .stroke(Color(nsColor: .separatorColor)))
                HStack(alignment: .top) {
                    Spacer()
                    Button("既定に戻す") {
                        name = AICommitSettings.defaults[index].name
                        prompt = AICommitSettings.defaults[index].prompt
                    }
                    .buttonStyle(.link)
                    .font(.caption)
                    .disabled(isDefault)
                }
            } label: {
                HStack(spacing: 6) {
                    HelpLabel(
                        title: "プロンプト",
                        help: "\(AICommitSettings.textPlaceholder) と書くとその位置に未確定文字列が"
                            + "入ります（無ければプロンプトに続けて渡されます）。")
                    if !expanded {
                        Text(prompt.replacingOccurrences(of: "\n", with: " "))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                            .truncationMode(.tail)
                    }
                }
            }
    }

    private var headerName: String {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? AICommitSettings.defaults[index].name : trimmed
    }

    private var isDefault: Bool {
        name == AICommitSettings.defaults[index].name
            && prompt == AICommitSettings.defaults[index].prompt
    }
}


/// AI変換のショートカット入力欄（"Option+Return" 形式。形式のチェックと、重なり・⌃Return の注意つき）。
/// 同じキーがほかのプリセットにあれば番号の小さいほうが、選択テキストのショートカットと同じなら
/// そちら（どのアプリでも先に受け取るグローバルショートカット）が動く
private struct AICommitShortcutField: View {
    let index: Int
    @Binding var hotkey: String
    // ほかのプリセットの欄を書き換えたときも重なりの注意を出し直すため、3つとも見張る（値は使わない）
    @AppStorage(AICommitSettings.hotkeyKey(0)) private var watch0 = ""
    @AppStorage(AICommitSettings.hotkeyKey(1)) private var watch1 = ""
    @AppStorage(AICommitSettings.hotkeyKey(2)) private var watch2 = ""

    // 入力欄の行は Form の行として直接並べる（VStack で包むと指定した幅が効かず、中身の長さで幅が変わる）
    var body: some View {
        HStack {
            Text("ショートカット")
            // labelsHidden: Form の中では空の見出しが幅を取り、入力欄が中身の長さまで縮むため
            TextField("", text: $hotkey, prompt: Text("例: Option+Return"))
                .labelsHidden()
                .textFieldStyle(.roundedBorder)
                .frame(width: 180)
            Group {
                if shortcut.isEmpty {
                    Text("なし").font(.caption).foregroundStyle(.secondary)
                } else if shortcut.isValid {
                    Image(systemName: "checkmark.circle").foregroundStyle(.green)
                } else {
                    Text("認識できない形式です").font(.caption).foregroundStyle(.red)
                }
            }
            Spacer()
        }
        ForEach(warnings, id: \.self) { warning in
            Text(warning).font(.caption).foregroundStyle(.orange)
        }
    }

    private var shortcut: AICommitShortcut { AICommitShortcut(rawValue: hotkey) }

    private var warnings: [String] {
        guard shortcut.isValid else { return [] }
        var result: [String] = []
        if let other = AICommitSettings.presets.first(where: {
            $0.index != index && $0.shortcut.isSameKey(as: shortcut)
        }) {
            let first = min(other.index, index) + 1
            result.append("\(other.index + 1). \(other.displayName) と同じキーです（\(first) 番が動きます）")
        }
        if SelectionSettings.isEnabled {
            let selectionHotkeys = SelectionSettings.presets
                .filter { $0.enabled && !$0.hotkey.isEmpty }.map(\.hotkey)
                + [SelectionSettings.onDemandHotkey]
            if selectionHotkeys.contains(where: { AICommitShortcut(rawValue: $0).isSameKey(as: shortcut) }) {
                result.append("選択テキストのショートカットと同じキーです（選択テキストのほうが動きます）")
            }
        }
        if shortcut.isControlReturn {
            result.append("⌃Returnは、テキストエディットなど多くのアプリでmacOSが右クリックメニューに使うため効きません")
        }
        return result
    }
}

/// "Ctrl+1" 形式のグローバルショートカット入力欄（形式チェック付き）
private struct HotkeyField: View {
    let label: String
    @Binding var hotkey: String

    var body: some View {
        HStack {
            Text(label)
            TextField("", text: $hotkey, prompt: Text("例: Ctrl+1"))
                .textFieldStyle(.roundedBorder)
                .frame(width: 140)
            if hotkey.trimmingCharacters(in: .whitespaces).isEmpty {
                Text("なし").font(.caption).foregroundStyle(.secondary)
            } else if GlobalShortcut.isValid(hotkey) {
                Image(systemName: "checkmark.circle").foregroundStyle(.green)
            } else {
                Text("認識できない形式です").font(.caption).foregroundStyle(.red)
            }
            Spacer()
        }
    }
}

/// 選択テキストプリセット1つ分の編集欄（有効・名前・ショートカット・プロンプト）
private struct SelectionPresetEditor: View {
    private let index: Int
    @AppStorage(SelectionSettings.enabledKey) private var groupEnabled = false
    @AppStorage private var enabled: Bool
    @AppStorage private var name: String
    @AppStorage private var prompt: String
    @AppStorage private var hotkey: String
    @State private var expanded = false

    init(index: Int) {
        self.index = index
        let defaults = SelectionSettings.defaults[index]
        _enabled = AppStorage(wrappedValue: defaults.enabled, SelectionSettings.enabledObjectKey(index))
        _name = AppStorage(wrappedValue: defaults.name, SelectionSettings.nameKey(index))
        _prompt = AppStorage(wrappedValue: defaults.prompt, SelectionSettings.promptKey(index))
        _hotkey = AppStorage(wrappedValue: defaults.hotkey, SelectionSettings.hotkeyKey(index))
    }

    var body: some View {
        // 機能全体がOFFのときは編集もまとめて無効にする
        Group {
            FormSubheader("プリセット\(index + 1). \(headerName)")
            HStack {
                Toggle("", isOn: $enabled)
                    .labelsHidden()
                Text("名前")
                TextField("", text: $name)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 140)
                Spacer()
                HotkeyField(label: "", hotkey: $hotkey)
                    .frame(width: 260)
            }

            DisclosureGroup(isExpanded: $expanded) {
                TextEditor(text: $prompt)
                    .font(.body)
                    .frame(height: 72)
                    .scrollContentBackground(.hidden)
                    .padding(4)
                    .overlay(
                        RoundedRectangle(cornerRadius: 6)
                            .stroke(Color(nsColor: .separatorColor)))
            } label: {
                HStack(spacing: 6) {
                    HelpLabel(
                        title: "プロンプト",
                        help: "\(AICommitSettings.textPlaceholder) と書くとその位置に選択テキストが"
                            + "入ります（無ければプロンプトに続けて渡されます）。")
                    if !expanded {
                        Text(prompt.replacingOccurrences(of: "\n", with: " "))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                            .truncationMode(.tail)
                    }
                }
            }
        }
        .disabled(!groupEnabled)
    }

    private var headerName: String {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? SelectionSettings.defaults[index].name : trimmed
    }
}

// MARK: - モデル（かな漢字変換モデル + AIサービス）

/// ファイルのパスの表示。書き換えはさせず「ファイルを選択...」で選ぶ（手で打つ必要がなく、打ち間違えると
/// 変換できなくなるため）。選んでコピーはできる。長いパスは折り返して全部見せる。空なら既定の説明を薄く出す
private struct PathDisplay: View {
    let path: String
    let placeholder: String

    var body: some View {
        Text(path.isEmpty ? placeholder : path)
            .foregroundStyle(path.isEmpty ? .secondary : .primary)
            .textSelection(.enabled)
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

private struct ModelSettingsTab: View {
    @AppStorage("modelPath") private var modelPath = ""
    @AppStorage(InferenceBackend.userDefaultsKey) private var backend = InferenceBackend.llamaCpp.rawValue
    @ObservedObject private var modelDownloader = ModelDownloader.shared

    /// 推論エンジンの状態の知らせ（選んだものと動いているものが違う理由）。無ければ nil
    private var backendNotice: (text: String, isWarning: Bool)? {
        let running = IrohaInputController.engineBackend
        let path = modelPath.isEmpty ? ZenzEngine.defaultModelPath : modelPath
        if backend == InferenceBackend.mlx.rawValue {
            if !InferenceBackend.isMLXSupportedHardware {
                return ("MLX は Apple Silicon の Mac でだけ使えます。llama.cpp で動きます。", true)
            }
            if FileManager.default.fileExists(atPath: path), !MLXConversionEngine.supports(modelPath: path) {
                return ("MLX は T5 のモデルにだけ対応しています。このモデルは llama.cpp で動きます。", true)
            }
        }
        if InferenceBackend.resolve(modelPath: path) != running {
            return ("変更は iroha の再起動後に反映されます。", false)
        }
        return nil
    }

    var body: some View {
        Form {
            Section("かな漢字変換モデル") {
                switch modelDownloader.state {
                case .downloading(let progress):
                    LabeledContent("モデルをダウンロード中") {
                        Text("\(Int(progress * 100))%").foregroundStyle(.secondary)
                    }
                case .failed(let reason):
                    Text("モデルのダウンロードに失敗しました: \(reason)（次回起動時に再試行します）")
                        .font(.caption)
                        .foregroundStyle(.red)
                default:
                    EmptyView()
                }
                LabeledContent("使用中のモデル") {
                    Text(IrohaInputController.engineModelDisplayName)
                        .foregroundStyle(.secondary)
                }
                Picker(selection: $backend) {
                    ForEach(InferenceBackend.allCases) { Text($0.displayName).tag($0.rawValue) }
                } label: {
                    HelpLabel(
                        title: "推論エンジン",
                        help: "モデルを動かす仕組みです。既定は llama.cpp です。"
                            + "MLX は T5 のモデルにだけ対応していて、1 回の変換が 2 割ほど速くなります（変換の結果は同じです）。"
                            + "zenz のモデルでは MLX を選んでも llama.cpp で動きます。"
                            + "変更は iroha の再起動後に反映されます。")
                }
                LabeledContent("使用中の推論エンジン") {
                    Text(IrohaInputController.engineBackend.displayName)
                        .foregroundStyle(.secondary)
                }
                if let notice = backendNotice {
                    Text(notice.text)
                        .font(.caption)
                        .foregroundStyle(notice.isWarning ? .orange : .secondary)
                }
                // 長いパスが切れないよう、ラベルは上に置いてパスに幅を全部使わせる
                VStack(alignment: .leading, spacing: 4) {
                    HelpLabel(title: "モデルファイル（GGUF）のパス", help: "モデルの変更はirohaの再起動後に反映されます。")
                    PathDisplay(path: modelPath, placeholder: ZenzEngine.defaultModelPath)
                    // 消えたモデルを指したままだと変換が一切できなくなるので、パスの下で知らせる
                    if !modelPath.isEmpty, !FileManager.default.fileExists(atPath: modelPath) {
                        Text("このパスにファイルがありません。変換できないので、モデルを選び直すか「既定に戻す」を押してください。")
                            .font(.caption)
                            .foregroundStyle(.red)
                    }
                }
                HStack {
                    Button("モデルフォルダを開く") {
                        let dir = DataDirectory.modelsURL
                        try? FileManager.default.createDirectory(
                            at: dir, withIntermediateDirectories: true)
                        NSWorkspace.shared.open(dir)
                    }
                    Button("ファイルを選択...") {
                        let panel = NSOpenPanel()
                        panel.allowedContentTypes = []
                        panel.allowsOtherFileTypes = true
                        panel.canChooseDirectories = false
                        panel.directoryURL = DataDirectory.modelsURL
                        if panel.runModal() == .OK, let url = panel.url {
                            modelPath = url.path
                        }
                    }
                    Button("既定に戻す") { modelPath = "" }
                        .disabled(modelPath.isEmpty)
                }
                Button("irohaを再起動") {
                    // 終了処理の詳細（_exitを使う理由等）はAppRestarterのコメントを参照
                    AppRestarter.restartInstalledApp()
                }
            }

            TrainingSection()

            AIServiceSection()
        }
        .formStyle(.grouped)
    }
}

// MARK: - 追加学習

/// 「自分の入力で追加学習」: 変換記録（ConversionLog）から LoRA アダプタを学習し、使うアダプタを選ぶ。
/// 学習そのものは別プロセス iroha-train（TrainingCoordinator）
private struct TrainingSection: View {
    @ObservedObject private var coordinator = TrainingCoordinator.shared
    @AppStorage(TrainingSettings.adapterPathKey) private var adapterPath = ""
    @AppStorage(ConversionLogSettings.enabledKey) private var conversionLogEnabled = false
    @AppStorage(TrainingSettings.epochsKey) private var epochs = TrainingSettings.defaultEpochs
    @AppStorage(TrainingSettings.learningRateKey) private var learningRate = TrainingSettings.defaultLearningRate

    private var basePath: String { IrohaInputController.engineModelPath }

    var body: some View {
        Section {
            if !TrainingCoordinator.isSupportedHardware {
                Text("追加学習は Apple Silicon の Mac で使えます。").foregroundStyle(.secondary)
            } else if !TrainingCoordinator.isAvailable {
                Text("学習ヘルパー（iroha-train）がこのバンドルにありません。").foregroundStyle(.secondary)
            } else {
                recordsRow
                parameterRows
                trainingRow
            }
            adapterField
        } header: {
            HelpSectionHeader(
                title: "自分の入力で追加学習",
                help: "確定した変換の記録（辞書・学習タブの「変換記録」）で、使用中のモデルに自分の文章の癖を"
                    + "追加学習します。ベースのモデルは変えず、小さなアダプタファイルを models/adapters/ に作ります。"
                    + "手順は 4 つ: ① 記録を1件ずつ今のモデルで変換し直す ② 間違えたものと正しかったものの一部を"
                    + "評価用に取り分ける ③ 残りの記録すべてで学習する ④ 評価用の記録をアダプタなし／ありで変換して"
                    + "結果を並べる。使った記録は .train.tsv / .mistakes.tsv / .correct.tsv としてアダプタの隣に残ります。"
                    + "学習は数十秒〜数分かかり、その間 GPU を使います。アダプタの変更は再起動後に反映されます。")
        }
        .onAppear { coordinator.refreshInfo(basePath: basePath) }
        .onReceive(NotificationCenter.default.publisher(for: ConversionLog.didChangeNotification)) { _ in
            coordinator.refreshInfo(basePath: basePath)
        }
    }

    @ViewBuilder private var recordsRow: some View {
        LabeledContent("記録") {
            if let info = coordinator.info {
                Text("\(info.usableEntries) 件").foregroundStyle(.secondary)
            } else if let error = coordinator.infoError {
                Text(error).foregroundStyle(.red).font(.caption)
            } else {
                ProgressView().controlSize(.small)
            }
        }
        // 学習の質は記録の量で決まる。足りない・少ないときは「貯めましょう」を前に出す
        if let info = coordinator.info, info.usableEntries < info.minimumRecords {
            Text("学習には \(info.minimumRecords) 件以上の記録が必要です（あと \(info.minimumRecords - info.usableEntries) 件）。")
                .font(.caption)
                .foregroundStyle(.secondary)
        } else {
            Text("記録が増えるほど、自分の文章に合った学習ができます。")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        if !conversionLogEnabled {
            Text("変換記録が OFF です。辞書・学習タブの「確定した変換を記録する」を ON にすると記録が溜まります。")
                .font(.caption)
                .foregroundStyle(.orange)
        }
        if let info = coordinator.info, !info.supported {
            Text("使用中のモデル（\(info.architecture.isEmpty ? "不明" : info.architecture)）は追加学習に対応していません"
                + "（対応: gpt2 = zenz、t5 = iroha の自作モデル）。")
                .font(.caption)
                .foregroundStyle(.orange)
        }
    }

    private var canStart: Bool {
        guard let info = coordinator.info else { return false }
        return info.supported && info.usableEntries >= info.minimumRecords
    }

    private var isIdle: Bool {
        if case .idle = coordinator.state { return true }
        return false
    }

    /// エポック数と学習率。常に見せる（折りたたみに隠すと見つからない）。学習中は変えられない
    /// エポック数・学習率の両方の説明に付ける共通の注意
    private var parameterCaution: String {
        "どちらも増やすほど強く覚えますが、元からできていた変換が崩れることもあります。"
            + "結果の「できていた変換」が減ったら弱めてください。既定はエポック \(TrainingSettings.defaultEpochs)・学習率 "
            + "\(TrainingSettings.format(learningRate: TrainingSettings.defaultLearningRate))。"
    }

    @ViewBuilder private var parameterRows: some View {
        LabeledContent {
            HStack(spacing: 4) {
                TextField("", value: $epochs, format: .number)
                    .textFieldStyle(.roundedBorder)
                    .multilineTextAlignment(.trailing)
                    .frame(width: 56)
                    .onChange(of: epochs) { _, value in
                        epochs = min(max(value, TrainingSettings.epochsRange.lowerBound), TrainingSettings.epochsRange.upperBound)
                    }
                Stepper("", value: $epochs, in: TrainingSettings.epochsRange).labelsHidden()
            }
            .disabled(!isIdle)
        } label: {
            HelpLabel(
                title: "エポック数",
                help: "記録全体を何周学習するか（\(TrainingSettings.epochsRange.lowerBound)〜"
                    + "\(TrainingSettings.epochsRange.upperBound)）。" + parameterCaution,
                isDisabled: !isIdle)
        }
        LabeledContent {
            HStack(spacing: 4) {
                LearningRateField(learningRate: $learningRate)
                Menu("プリセット") {
                    ForEach(TrainingSettings.learningRateChoices, id: \.value) { choice in
                        Button(choice.label) { learningRate = choice.value }
                    }
                }
                .frame(width: 110)
            }
            .disabled(!isIdle)
        } label: {
            HelpLabel(
                title: "学習率",
                help: "1e-4 や 0.0001 のように入力します"
                    + "（\(TrainingSettings.format(learningRate: TrainingSettings.learningRateRange.lowerBound))"
                    + "〜\(TrainingSettings.format(learningRate: TrainingSettings.learningRateRange.upperBound))）。"
                    + parameterCaution,
                isDisabled: !isIdle)
        }
    }

    @ViewBuilder private var trainingRow: some View {
        switch coordinator.state {
        case .idle:
            Button("学習を開始") { coordinator.start(basePath: basePath) }
                .disabled(!canStart)
        case .running(let stage, let step, let progress):
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    if let step, step.steps > 0 {
                        ProgressView(value: Double(step.step), total: Double(step.steps))
                    } else if let progress, progress.total > 0 {
                        ProgressView(value: Double(progress.done), total: Double(progress.total))
                    } else {
                        ProgressView().controlSize(.small)
                    }
                    Button("キャンセル") { coordinator.cancel() }
                }
                Text(Self.describe(stage: stage, step: step, progress: progress))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        case .done(let result):
            TrainingResultView(result: result, adapterPath: $adapterPath, onClose: { coordinator.reset() })
        case .failed(let message):
            VStack(alignment: .leading, spacing: 4) {
                Text("学習に失敗しました: \(message)").font(.caption).foregroundStyle(.red)
                Button("戻る") { coordinator.reset() }
            }
        case .cancelled:
            HStack {
                Text("学習を中止しました。").foregroundStyle(.secondary)
                Button("戻る") { coordinator.reset() }
            }
        }
    }

    private static func describe(stage: String, step: TrainingStep?,
                                 progress: TrainingCoordinator.Progress?) -> String {
        switch stage {
        case "start": return "準備中…"
        case "screen":
            guard let progress else { return "① 記録を今のモデルで変換し直しています…" }
            return "① 記録を今のモデルで変換し直しています… \(progress.done)/\(progress.total) 件"
        case "evaluate": return "④ 評価用の記録をアダプタなし／ありで変換しています…"
        case "quantize": return "ベースモデルを学習用に変換中…"
        case "load": return "モデルを読み込み中…"
        case "export": return "アダプタを書き出し中…"
        default:
            guard let step else { return "③ 学習中…" }
            var text = "③ 学習中 \(step.step)/\(step.steps)"
            if step.epochs > 0 { text += "（エポック \(step.epoch)/\(step.epochs)）" }
            text += String(format: "  損失 %.3f", step.loss)
            return text
        }
    }

    @ViewBuilder private var adapterField: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("使用する LoRA アダプタ（GGUF）のパス")
            PathDisplay(path: adapterPath, placeholder: "なし（ベースモデルのまま）")
        }
        HStack {
            Button("アダプタフォルダを開く") {
                let dir = TrainingCoordinator.adaptersDirectory
                try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                NSWorkspace.shared.open(dir)
            }
            Button("ファイルを選択...") {
                let panel = NSOpenPanel()
                panel.allowedContentTypes = []
                panel.allowsOtherFileTypes = true
                panel.canChooseDirectories = false
                panel.directoryURL = TrainingCoordinator.adaptersDirectory
                if panel.runModal() == .OK, let url = panel.url {
                    adapterPath = url.path
                }
            }
            Button("使わない") { adapterPath = "" }
                .disabled(adapterPath.isEmpty)
        }
        if adapterPath != (IrohaInputController.engineAdapterPath ?? "") {
            HStack {
                Text("アダプタの変更はirohaの再起動後に反映されます。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Button("irohaを再起動") { AppRestarter.restartInstalledApp() }
            }
        }
    }
}

/// 学習率の入力欄。"1e-4" のような指数表記で見せ、確定時に解釈できて範囲内なら保存、だめなら元の値に戻す
private struct LearningRateField: View {
    @Binding var learningRate: Double
    @State private var text = ""
    @FocusState private var focused: Bool

    var body: some View {
        TextField("", text: $text)
            .textFieldStyle(.roundedBorder)
            .multilineTextAlignment(.trailing)
            .frame(width: 80)
            .focused($focused)
            .onAppear { text = TrainingSettings.format(learningRate: learningRate) }
            .onChange(of: learningRate) { _, value in text = TrainingSettings.format(learningRate: value) }
            .onSubmit(commit)
            .onChange(of: focused) { _, isFocused in if !isFocused { commit() } }
    }

    private func commit() {
        if let value = TrainingSettings.parse(learningRate: text) {
            learningRate = value
        }
        text = TrainingSettings.format(learningRate: learningRate)
    }
}

/// 学習結果: 評価用の記録をアダプタなし／ありで変換した一致数を 2 群（間違えていた変換・できていた変換）で並べる。
/// 体感では悪化の方が目立つので、効果だけでなく副作用も必ず見せる
private struct TrainingResultView: View {
    let result: TrainingResult
    @Binding var adapterPath: String
    let onClose: () -> Void

    private var elapsedText: String {
        let seconds = Int(result.elapsed.rounded())
        return seconds >= 60 ? "\(seconds / 60)分\(seconds % 60)秒" : "\(seconds)秒"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("学習が終わりました（記録 \(result.data.records) 件・\(elapsedText)）").font(.headline)
            Text("記録のうち \(result.data.trainLines) 件で学習し、"
                + "\(result.data.heldOutMistakes + result.data.heldOutCorrect) 件は学習に使わず評価用に残しました。"
                + "その評価用の記録を、アダプタなし／ありで変換して記録と一致した数を並べています。")
                .font(.caption)
                .foregroundStyle(.secondary)
            if result.after.mistakes.total > 0 {
                scoreRow(title: "今のモデルが間違えていた記録", before: result.before.mistakes, after: result.after.mistakes,
                         help: "アダプタなしでは記録と違う変換になっていた記録。増えていれば、間違いが直った数")
            } else {
                Text("今のモデルは記録をすべて正しく変換できていたので、「間違いが直ったか」は測れません。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            if result.after.correct.total > 0 {
                scoreRow(title: "今のモデルが正しく変換できていた記録", before: result.before.correct, after: result.after.correct,
                         help: "アダプタなしで記録どおりに変換できていた記録（新しい方から最大 \(TrainingDataBuilder.maxHeldOutCorrect) 件）。"
                            + "減っていれば、学習で崩れた数")
            }
            if result.after.mistakes.total > 0, result.after.mistakes.total < 10 {
                Text("評価に回せた間違いが \(result.after.mistakes.total) 件と少ないので、この数値はぶれます。"
                    + "記録が溜まるほど確かになります。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Text(URL(fileURLWithPath: result.adapter).lastPathComponent)
                .font(.caption)
                .foregroundStyle(.secondary)
            HStack {
                Button("このアダプタを使う") { adapterPath = result.adapter }
                    .disabled(adapterPath == result.adapter)
                Button("閉じる", action: onClose)
            }
        }
    }

    /// 「N 件   アダプタなし a 件正解 → あり b 件正解（+2）」の 1 行
    @ViewBuilder private func scoreRow(title: String, before: TrainingScore, after: TrainingScore, help: String) -> some View {
        let delta = after.exact - before.exact
        LabeledContent("\(title) \(after.total) 件") {
            HStack(spacing: 6) {
                Text("アダプタなし \(before.exact) 件正解").foregroundStyle(.secondary)
                Text("→")
                Text("あり \(after.exact) 件正解")
                Text(delta == 0 ? "変化なし" : (delta > 0 ? "+\(delta)" : "\(delta)"))
                    .foregroundStyle(delta == 0 ? .secondary : (delta > 0 ? Color.green : Color.orange))
            }
            .font(.callout)
        }
        .help(help)
    }
}

// MARK: - 情報

/// ライセンス表示の1行（名称・作者・ライセンス名と、配布元へのリンク）
private struct LicenseRow: View {
    var name: String
    var holder: String
    var license: String
    var note: String? = nil
    var url: String

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(name).fontWeight(.medium)
                if let note {
                    Text(note).font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Text(license).foregroundStyle(.secondary)
            }
            HStack(spacing: 8) {
                Text("© \(holder)")
                if let destination = URL(string: url) {
                    Link(url, destination: destination)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
            }
            .font(.caption)
            .foregroundStyle(.secondary)
        }
        .padding(.vertical, 2)
    }
}

/// 打ち間違い訂正モデルとその学習元のライセンス（入れているときだけ出す）。
///
/// 学習元はモデルごとに違うので、アプリに焼き込まずカタログ（入れたときの記録）から取る
private struct TypoNormalizerLicenseRows: View {
    @ObservedObject private var downloader = TypoNormalizerDownloader.shared

    var body: some View {
        Group { rows }
            // 学習元を記録する前に入れたモデルは、カタログから取る（入れていなければ通信しない）
            .onAppear {
                if let installed = downloader.installed, installed.sources == nil {
                    downloader.refreshCatalogIfNeeded()
                }
            }
    }

    @ViewBuilder
    private var rows: some View {
        if let installed = downloader.installedLicense {
            LicenseRow(
                name: "打ち間違い訂正モデル（\(installed.id)）", holder: "Tetsuaki Baba",
                license: installed.license ?? "",
                url: installed.page ?? "https://github.com/TetsuakiBaba/iroha")
            ForEach(installed.sources, id: \.name) { source in
                LicenseRow(
                    name: source.name, holder: source.holder, license: source.license,
                    note: "打ち間違い訂正モデルの学習元", url: source.url)
            }
            if !installed.sources.isEmpty {
                Text("学習元のデータは、文への分割・読みの付与など加工して学習に使っています。"
                    + "データそのものはアプリにもモデルにも含めていません。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

/// データの保存場所（設定 > 情報）。iCloud Drive / Dropbox のフォルダを指定して他のMacと共有する
private struct DataDirectorySection: View {
    @State private var pendingURL: URL?
    @State private var errorMessage: String?

    var body: some View {
        Section {
            VStack(alignment: .leading, spacing: 4) {
                Text("フォルダ")
                Text(DataDirectorySettings.displayPath)
                    .font(.callout.monospaced())
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
                    .lineLimit(2)
                    .truncationMode(.middle)
            }
            HStack {
                Button("フォルダを変更...") { chooseFolder() }
                Button("既定の場所に戻す") { pendingURL = DataDirectory.defaultURL }
                    .disabled(DataDirectory.isDefault)
                Button("Finderで開く") {
                    try? FileManager.default.createDirectory(
                        at: DataDirectory.url, withIntermediateDirectories: true)
                    NSWorkspace.shared.open(DataDirectory.url)
                }
            }
        } header: {
            HelpSectionHeader(
                title: "データの保存場所",
                help: "ユーザ辞書・変換ルール・学習・変換記録・変換モデル・設定をこのフォルダに保存します。"
                    + "iCloud DriveやDropboxのフォルダを指定すると、複数のMacで同じデータを共有できます。"
                    + "変更するとirohaが再起動します。")
        }
        .alert(
            "データの保存場所を変更しますか？",
            isPresented: Binding(get: { pendingURL != nil }, set: { if !$0 { pendingURL = nil } }),
            presenting: pendingURL
        ) { url in
            Button("今のデータをコピーして変更") { apply(url, copyExisting: true) }
            Button("コピーせずに変更") { apply(url, copyExisting: false) }
            Button("キャンセル", role: .cancel) {}
        } message: { url in
            let path = (url.path as NSString).abbreviatingWithTildeInPath
            if DataDirectorySettings.hasExistingData(at: url) {
                Text("\(path)\n\nこのフォルダには既にirohaのデータがあります。"
                     + "「コピーして変更」では、そこに無いファイルだけを今の場所からコピーします"
                     + "（既にあるファイルは上書きしません）。変更後にirohaを再起動します。")
            } else {
                Text("\(path)\n\n変更後にirohaを再起動します。")
            }
        }
        .alert("保存場所を変更できませんでした", isPresented: Binding(
            get: { errorMessage != nil }, set: { if !$0 { errorMessage = nil } })
        ) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(errorMessage ?? "")
        }
    }

    private func chooseFolder() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = "このフォルダを使う"
        panel.message = "irohaのデータを保存するフォルダを選んでください（例: iCloud DriveやDropboxの中の「iroha」フォルダ）"
        panel.directoryURL = DataDirectory.url
        panel.level = .modalPanel
        guard panel.runModal() == .OK, let url = panel.url else { return }
        guard url.standardizedFileURL.path != DataDirectory.url.standardizedFileURL.path else { return }
        pendingURL = url
    }

    private func apply(_ url: URL, copyExisting: Bool) {
        do {
            try DataDirectorySettings.change(to: url, copyExisting: copyExisting)
        } catch {
            errorMessage = error.localizedDescription
            return
        }
        // 終了処理の詳細（_exitを使う理由等）はAppRestarterのコメントを参照
        AppRestarter.restartInstalledApp()
    }
}

private struct AboutSettingsTab: View {
    @AppStorage("autoUpdateCheck") private var autoUpdateCheck = true
    @AppStorage(DeveloperOverlaySettings.enabledKey) private var developerOverlay = false
    @State private var showingUninstallConfirm = false

    var body: some View {
        Form {
            Section("アップデート") {
                Toggle("自動でアップデートを確認", isOn: $autoUpdateCheck)
                HStack {
                    Button("今すぐ確認") {
                        Task { await UpdateChecker.shared.checkAndPresent() }
                    }
                    Spacer()
                    if let last = UserDefaults.standard.object(forKey: "lastUpdateCheckDate") as? Date {
                        Text("前回の確認: \(last.formatted(date: .abbreviated, time: .shortened))")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }

            Section("iroha") {
                LabeledContent("バージョン") {
                    Text(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?")
                }
            }

            DataDirectorySection()

            Section("ライセンス") {
                LicenseRow(
                    name: "iroha", holder: "Tetsuaki Baba", license: "MIT License",
                    url: "https://github.com/TetsuakiBaba/iroha")
                LicenseRow(
                    name: "zenz-v3.1", holder: "Keita Miwa", license: "CC BY-SA 4.0",
                    note: "既定の変換モデル",
                    url: "https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf")
                LicenseRow(
                    name: "llama.cpp", holder: "ggml-org", license: "MIT License",
                    note: "変換モデルの推論エンジン",
                    url: "https://github.com/ggml-org/llama.cpp")
                LicenseRow(
                    name: "AzooKeyKanaKanjiConverter", holder: "ensan (azooKey)", license: "MIT License",
                    note: "候補ウィンドウの辞書ラティス",
                    url: "https://github.com/azooKey/AzooKeyKanaKanjiConverter")
                LicenseRow(
                    name: "azooKey_dictionary_storage", holder: "azooKey", license: "Apache License 2.0",
                    note: "辞書データ",
                    url: "https://github.com/azooKey/azooKey_dictionary_storage")
                LicenseRow(
                    name: "swift-algorithms / swift-collections / swift-tokenizers",
                    holder: "Apple, ensan", license: "Apache License 2.0",
                    note: "AzooKeyKanaKanjiConverterの依存",
                    url: "https://github.com/apple/swift-collections")
                LicenseRow(
                    name: "Tsukimi Rounded", holder: "Takashi Funayama",
                    license: "SIL Open Font License 1.1",
                    note: "アプリアイコン・メニューバーアイコンの書体",
                    url: "https://fonts.google.com/specimen/Tsukimi+Rounded")
                TypoNormalizerLicenseRows()
            }

            Section("開発者向け") {
                HelpToggle(
                    title: "推論にかかった時間と左文脈をカーソルの右下に表示する", isOn: $developerOverlay,
                    help: "開発者向けの表示です。入力を始めたときに、カーソルの左の文字をアプリから読めたか"
                        + "（読めなければその理由と、代わりに使う確定済みの文字列）を出します。"
                        + "かな漢字変換と打ち間違いの訂正を実行するたびに、かかった時間を出します。"
                        + "「全体」は変換を頼んでから結果が返るまで、「NN」はそのうちニューラルネットの計算だけの時間です"
                        + "（差は辞書・学習の処理と、先に走っている推論の待ち時間）。"
                        + "「取り消し」は、前の表示のあと次の入力で打ち切った変換の数です。この設定は他のMacと同期しません。")
            }

            Section("アンインストール") {
                HStack(spacing: 4) {
                    Button("irohaをアンインストール...", role: .destructive) {
                        showingUninstallConfirm = true
                    }
                    HelpButton("入力ソースの一覧からirohaを外し、アプリ本体を削除して終了します。")
                }
            }
        }
        .formStyle(.grouped)
        .alert("irohaをアンインストールしますか？", isPresented: $showingUninstallConfirm) {
            Button("アプリのみ削除", role: .destructive) {
                Uninstaller.run(purgeData: false)
            }
            Button("データも含めて削除", role: .destructive) {
                Uninstaller.run(purgeData: true)
            }
            Button("キャンセル", role: .cancel) {}
        } message: {
            Text("入力ソースからirohaを外し、~/Library/Input Methods/iroha.app を削除して"
                + "終了します。「データも含めて削除」を選ぶと、ユーザ辞書・変換ルール・学習・"
                + "変換記録・変換モデル・設定・APIキーも削除します。")
        }
    }
}
