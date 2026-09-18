import ApplicationServices
import SwiftUI
import IrohaCore

/// 設定ウィンドウのタブ
enum SettingsTab: Hashable {
    case input       // 入力・変換のふるまい
    case dictionary  // ユーザ辞書・変換ルール・変換の学習・変換記録
    case selection   // 他アプリの選択テキストのAI編集 + 選択した文字数の表示
    case model       // かな漢字変換・予測に使うモデルとAIサービス
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
    }
}

// MARK: - 入力

private struct InputSettingsTab: View {
    @AppStorage("liveConversion") private var liveConversion = true
    @AppStorage("commitOnPunctuation") private var commitOnPunctuation = false
    @AppStorage(DocumentContextSettings.enabledKey) private var documentContext = true
    @AppStorage("candidateCount") private var candidateCount = 8
    @AppStorage("punctuationStyle") private var punctuationStyle = "、。"
    @AppStorage(PredictionSettings.predictiveEnabledKey) private var predictiveConversion = false
    @AppStorage(PredictionSettings.completionEnabledKey) private var inlineCompletion = false
    @AppStorage(PredictionSettings.delayMillisecondsKey)
    private var predictionDelayMs = PredictionSettings.defaultDelayMilliseconds

    var body: some View {
        Form {
            Section("変換") {
                Toggle("ライブ変換", isOn: $liveConversion)
                Toggle("句読点で自動確定", isOn: $commitOnPunctuation)
                    .disabled(!liveConversion)
                Stepper(value: $candidateCount, in: 3...16) {
                    HStack {
                        Text("候補ウィンドウでモデルが並べる候補数")
                        Spacer()
                        Text("\(candidateCount)").foregroundStyle(.secondary)
                    }
                }
                Text("この数の下に、読みが一致する辞書の残りの候補（単漢字・異体字・人名など）が続きます。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Toggle("アプリの文章を文脈に使う", isOn: $documentContext)
                Text("入力を始めた位置の手前にある文章（最大40文字）をアプリから読み取り、変換と予測の文脈にします。"
                    + "文章の途中に書き足すときや、別のアプリに移った直後でも前後に合った変換になります。"
                    + "文章を返さないアプリでは、irohaで直前に確定した文字列を文脈にします。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("予測") {
                Toggle("予測変換（入力中）", isOn: $predictiveConversion)
                    .disabled(!liveConversion)
                Text("入力を止めると、変換結果の続き（次の文節）をカーソルの下の小さなウィンドウに表示します。"
                    + "Tabで取り入れ、そのまま入力を続けられます。取り入れた部分はBackspaceで取り消せます。"
                    + "ライブ変換がONのときだけ動きます。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Toggle("インライン補完（確定後）", isOn: $inlineCompletion)
                Text("確定したあと操作を止めると、文章の続き（次の文節）を同じウィンドウに表示します。"
                    + "Tabで確定、それ以外のキーで消えます。Tabを押すまでアプリの文字は変わりません。"
                    + "句読点が出たらそこまでを予測します。使うモデルは「モデル」タブで変えられます。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                PredictionDelayRow(milliseconds: $predictionDelayMs)
                    .disabled(!predictiveConversion && !inlineCompletion)
                Text("キーを離してからこの時間だけ何も押さなければ予測を出します。短いほど早く出ますが、"
                    + "入力中に頻繫に出て煩わしくなります。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("句読点") {
                Picker("句読点スタイル", selection: $punctuationStyle) {
                    Text("、。").tag("、。")
                    Text("，．").tag("，．")
                }
                .pickerStyle(.segmented)
                Text("変更は次の入力から反映されます。入力中は ⌃.（control + ピリオド）でも切り替えられます。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            // 未確定文字列をAIに渡して確定する（使うAIサービスは「モデル」タブで選ぶ）
            Section("AI変換して確定") {
                Text("修飾キー+Returnで、入力中の未確定文字列をAIに渡し、返ってきた結果を確定します。"
                    + "使うAIサービスは「モデル」タブで選びます。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                AICommitPresetEditor(index: 0)
                AICommitPresetEditor(index: 1)
                AICommitPresetEditor(index: 2)
            }
        }
        .formStyle(.grouped)
    }
}

/// 予測を出すまでの休止時間（ミリ秒）のスライダー行
private struct PredictionDelayRow: View {
    @Binding var milliseconds: Int
    private static let step = 50.0
    private static let range = Double(PredictionSettings.delayMillisecondsRange.lowerBound)
        ... Double(PredictionSettings.delayMillisecondsRange.upperBound)

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("予測を出すまでの休止時間")
                Spacer()
                Text("\(milliseconds) ms")
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
            }
            Slider(value: sliderValue, in: Self.range, step: Self.step)
        }
    }

    private var sliderValue: Binding<Double> {
        Binding(
            get: { Double(milliseconds) },
            set: { milliseconds = Int(($0 / Self.step).rounded() * Self.step) }
        )
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
                Toggle("起動時にmacOSのユーザ辞書を取り込む", isOn: $syncSystemDictionary)
                Text("「システム設定 > キーボード > ユーザ辞書」に登録した単語を取り込みます"
                    + "（読み取りのみ。macOS側の辞書は変更しません）。"
                    + "取り込んだ単語をirohaで編集すると、以後の取り込みでは上書きされません。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("変換ルール") {
                LabeledContent("登録ルール") {
                    HStack {
                        Text("\(rewriteRuleCount) 件").foregroundStyle(.secondary)
                        Button("編集...") { uiState.showingRewriteRules = true }
                    }
                }
                Text("トリガー（よみ）と出力の組を登録すると、文節の読みがトリガーに一致したとき"
                    + "出力を変換候補に加えます。出力には {{date:yyyy/MM/dd}} や {{time:HH:mm}} の"
                    + "ようなプレースホルダを書けて、変換のたびに今の日付・時刻に置き換わります"
                    + "（例:「きょう」→ 2026/09/06）。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("変換の学習") {
                Toggle("変換の修正を学習する", isOn: $learningEnabled)
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
                Text("文節変換（スペースキー）で候補を選び直して確定すると、入力した読みの全体と"
                    + "確定した文字列を覚えて、次に同じ読みを入力したとき最初に出します"
                    + "（「きしゃ」を「貴社」に直すと、次から「きしゃ」はそう変換されます）。"
                    + "読みの一部には当てはめません。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("変換記録") {
                Toggle("確定した変換を記録する", isOn: $conversionLogEnabled)
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
                Text("確定した変換を、そのときモデルに渡した文脈（カーソル手前の文章の末尾40文字）・読み・"
                    + "モデルの出力・確定した文字列とともに1件ずつ記録します。"
                    + "追加学習は「この文脈でこの読みならこう変換する」を学ぶので、"
                    + "左文脈のない確定（起動直後やフォーカス移動直後の1語目）は記録しません。"
                    + "「確認・編集...」で中身を見て、打ち間違いをそのまま確定した行は直すか削除できます。"
                    + "記録はデータフォルダ内の logs/conversions/ にこのMacのファイルとして残るだけで、"
                    + "どこにも送信されません。あとでこの記録を使って、自分の入力に合わせた変換モデルの"
                    + "追加学習（LoRAなど）ができます。上の「変換の学習」とは別のもので、"
                    + "記録しても変換の動作は変わりません。書いていた文章の一部がそのまま残るため、"
                    + "この設定は他のMacには同期されません。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
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
            Section("選択テキストのAI編集") {
                SelectionIntroRows(selectionEnabled: $selectionEnabled)
                Text("処理に使うAIサービス（Apple Intelligence・Ollamaなど）は「モデル」タブで設定します。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("マウスで選択したとき") {
                Picker("トリガー", selection: $triggerMode) {
                    ForEach(SelectionTriggerMode.allCases) { mode in
                        Text(mode.label).tag(mode.rawValue)
                    }
                }
                .disabled(!selectionEnabled)
            }

            Section("その場でAIに指示") {
                HotkeyField(label: "ショートカット", hotkey: $onDemandHotkey)
                    .disabled(!selectionEnabled)
                Text("選択テキストに自由な指示を出せます（選択なしで押すとテキスト生成になります）。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("プリセット") {
                Text("プリセットのショートカットは、テキストを選択していないときに押すと"
                    + "「テキストを生成」の入力欄になり、結果をカーソル位置へ挿入します。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                SelectionPresetEditor(index: 0)
                SelectionPresetEditor(index: 1)
                SelectionPresetEditor(index: 2)
                SelectionPresetEditor(index: 3)
                SelectionPresetEditor(index: 4)
            }

            Section("除外するアプリ") {
                TextField(
                    "", text: $excludedBundleIds,
                    prompt: Text("com.example.app, com.example.other"))
                    .textFieldStyle(.roundedBorder)
                    .disabled(!selectionEnabled)
                Text("ここに書いたバンドルIDのアプリでは、マウス選択のトリガーを出しません"
                    + "（カンマまたは改行区切り）。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            // AI編集とは独立した機能（マウスで選択した文字数を選択範囲の近くに出す）。
            // アクセシビリティ権限と除外するアプリの設定はAI編集と共通なのでこのタブに置く
            Section("選択した文字数の表示") {
                Toggle("選択した文字数を表示する", isOn: $characterCount)
                Text("マウスで選択（ドラッグ・ダブルクリック）すると、選択範囲の近くに文字数を数秒表示します"
                    + "（改行は数えず、空白があれば空白を除いた数も併記）。"
                    + "AI編集がONのときはアイコンに添えて表示します。"
                    + "アクセシビリティ権限が必要で、除外するアプリの設定も共通です。"
                    + "キーボードでの選択（Shift+矢印・⌘A）には反応しません。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
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
        Toggle("選択テキストのAI編集を有効にする", isOn: $selectionEnabled)
            .onAppear { accessibilityGranted = AXIsProcessTrusted() }
        Text("どのアプリでも、選択したテキストをショートカットやマウス操作からAIで"
            + "処理して置き換えられます。"
            + "ショートカットはirohaが起動している間だけ有効です"
            + "（ログイン後に一度日本語入力すると起動します）。")
            .font(.caption)
            .foregroundStyle(.secondary)
        LabeledContent("アクセシビリティ権限") {
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
        }
        Text("選択テキストの取得と置換にアクセシビリティ権限が必要です。")
            .font(.caption)
            .foregroundStyle(.secondary)
    }
}

/// 予測変換・インライン補完に使うモデル（GGUF）のパス入力欄（空ならかな漢字変換と同じモデル）
private struct PredictionModelPathField: View {
    let title: String
    let key: String
    @State private var path: String

    init(title: String, key: String) {
        self.title = title
        self.key = key
        _path = State(initialValue: UserDefaults.standard.string(forKey: key) ?? "")
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(title)
                Spacer()
                Text(PredictionSettings.modelDisplayName(forKey: key))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            HStack {
                TextField("", text: $path, prompt: Text("かな漢字変換と同じモデル"))
                    .textFieldStyle(.roundedBorder)
                    .onChange(of: path) { _, newValue in
                        UserDefaults.standard.set(newValue, forKey: key)
                    }
                Button("選択...") {
                    let panel = NSOpenPanel()
                    panel.allowedContentTypes = []
                    panel.allowsOtherFileTypes = true
                    panel.canChooseDirectories = false
                    panel.directoryURL = DataDirectory.modelsURL
                    if panel.runModal() == .OK, let url = panel.url {
                        path = url.path
                    }
                }
            }
        }
    }
}

/// AIサービス（バックエンド）の設定セクション。「モデル」タブに置き、
/// 入力タブの「AI変換して確定」と選択テキストのAI編集が共通で使う
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
            Picker("サービス", selection: $translationService) {
                Text("Apple Intelligence（オンデバイス）").tag("apple")
                Text("Ollama").tag("ollama")
                Text("LM Studio").tag("lmstudio")
                Text("OpenAI互換（外部API）").tag("openai")
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
            Text(translationCaption)
                .font(.caption)
                .foregroundStyle(.secondary)
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
        let common = "「AI変換して確定」と「選択テキストのAI編集」はここで選んだサービスを使います。"
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
    @AppStorage private var shortcut: String
    @State private var expanded = false

    init(index: Int) {
        self.index = index
        let defaults = AICommitSettings.defaults[index]
        _name = AppStorage(wrappedValue: defaults.name, AICommitSettings.nameKey(index))
        _prompt = AppStorage(wrappedValue: defaults.prompt, AICommitSettings.promptKey(index))
        _shortcut = AppStorage(
            wrappedValue: defaults.shortcut.rawValue, AICommitSettings.shortcutKey(index))
    }

    var body: some View {
        FormSubheader("\(index + 1). \(headerName)")
            // 同じショートカットを複数のプリセットに割り当てられないようにする
            .onChange(of: shortcut) {
                guard shortcut != AICommitShortcut.off.rawValue else { return }
                for other in 0..<AICommitSettings.count where other != index {
                    let key = AICommitSettings.shortcutKey(other)
                    let current = UserDefaults.standard.string(forKey: key)
                        ?? AICommitSettings.defaults[other].shortcut.rawValue
                    if current == shortcut {
                        UserDefaults.standard.set(AICommitShortcut.off.rawValue, forKey: key)
                    }
                }
            }
            HStack {
                // ラベルを別に置く（TextFieldのタイトルにすると値が右寄せになる）
                Text("名前")
                TextField("", text: $name)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 140)
                Spacer()
                Picker("", selection: $shortcut) {
                    ForEach(AICommitShortcut.allCases) { option in
                        Text(option.label).tag(option.rawValue)
                    }
                }
                .labelsHidden()
                .frame(maxWidth: 250)
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
                HStack(alignment: .top) {
                    Text("\(AICommitSettings.textPlaceholder) と書くとその位置に未確定文字列が"
                        + "入ります（無ければプロンプトに続けて渡されます）。")
                        .font(.caption)
                        .foregroundStyle(.secondary)
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
                    Text("プロンプト")
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
                Text("\(AICommitSettings.textPlaceholder) と書くとその位置に選択テキストが"
                    + "入ります（無ければプロンプトに続けて渡されます）。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } label: {
                HStack(spacing: 6) {
                    Text("プロンプト")
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

private struct ModelSettingsTab: View {
    @AppStorage("modelPath") private var modelPath = ""
    @ObservedObject private var modelDownloader = ModelDownloader.shared

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
                // 長いパスが切れないよう、ラベルは上に置いて入力欄に幅を全部使わせる
                VStack(alignment: .leading, spacing: 4) {
                    Text("モデルファイル（GGUF）のパス")
                    TextField("", text: $modelPath, prompt: Text(ZenzEngine.defaultModelPath))
                        .textFieldStyle(.roundedBorder)
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
                }
                Text("モデルの変更はirohaの再起動後に反映されます。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Button("irohaを再起動") {
                    // 終了処理の詳細（_exitを使う理由等）はAppRestarterのコメントを参照
                    AppRestarter.restartInstalledApp()
                }
            }

            TrainingSection()

            Section("予測変換・インライン補完のモデル") {
                Text("入力中の予測変換と確定後のインライン補完は、かな漢字変換とは別のモデルを使えます。"
                    + "空欄ならかな漢字変換と同じモデルを共有します（zenz-v3は文章の続きも生成できます）。"
                    + "変更はirohaの再起動後に反映されます。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                PredictionModelPathField(
                    title: "予測変換（入力中）", key: PredictionSettings.predictiveModelPathKey)
                PredictionModelPathField(
                    title: "インライン補完（確定後）", key: PredictionSettings.completionModelPathKey)
            }

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

    private var basePath: String { IrohaInputController.engineModelPath }

    var body: some View {
        Section("自分の入力で追加学習") {
            if !TrainingCoordinator.isSupportedHardware {
                Text("追加学習は Apple Silicon の Mac で使えます。").foregroundStyle(.secondary)
            } else if !TrainingCoordinator.isAvailable {
                Text("学習ヘルパー（iroha-train）がこのバンドルにありません。").foregroundStyle(.secondary)
            } else {
                recordsRow
                trainingRow
            }
            adapterField
            Text("確定した変換の記録（辞書・学習タブの「変換記録」）を使って、使用中のモデルに LoRA アダプタを"
                + "追加学習します。ベースのモデルは変えず、小さなアダプタファイルを models/adapters/ に作ります。"
                + "まず記録を1件ずつ変換し直して、いまのモデルが間違えるものを選り分け、それを重点的に学習します"
                + "（正解できている記録は忘れ防止に少量混ぜます）。間違いの新しい方から 2 割は学習に使わず、"
                + "学習後に「間違いが直った数」と「元から正しかった変換を保てた数」を測るのに使います。"
                + "使った記録はアダプタと並べて .train.tsv / .mistakes.tsv に残るので、何を覚えさせたか確かめられます。"
                + "学習は数十秒〜数分かかり、その間 GPU を使います。アダプタの変更は再起動後に反映されます。")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .onAppear { coordinator.refreshInfo(basePath: basePath) }
        .onReceive(NotificationCenter.default.publisher(for: ConversionLog.didChangeNotification)) { _ in
            coordinator.refreshInfo(basePath: basePath)
        }
    }

    @ViewBuilder private var recordsRow: some View {
        LabeledContent("記録") {
            if let info = coordinator.info {
                // 学習対象は学習時に変換し直して選ぶので、ここでは件数の目安だけを出す
                Text("\(info.usableEntries) 件（自分で直した確定 \(info.corrections) 件）")
                    .foregroundStyle(.secondary)
            } else if let error = coordinator.infoError {
                Text(error).foregroundStyle(.red).font(.caption)
            } else {
                ProgressView().controlSize(.small)
            }
        }
        if !conversionLogEnabled {
            Text("変換記録が OFF です。辞書・学習タブの「確定した変換を記録する」を ON にすると記録が溜まります。")
                .font(.caption)
                .foregroundStyle(.orange)
        }
        if let info = coordinator.info, !info.supported {
            Text("使用中のモデル（\(info.architecture.isEmpty ? "不明" : info.architecture)）は追加学習に対応していません"
                + "（対応: gpt2 = zenz）。")
                .font(.caption)
                .foregroundStyle(.orange)
        }
    }

    private var canStart: Bool {
        guard let info = coordinator.info else { return false }
        // 学習できる間違いの数は実際に変換してみるまで分からないので、ここでは記録の件数で判断する
        return info.supported && info.usableEntries >= info.minimumRecords
    }

    @ViewBuilder private var trainingRow: some View {
        switch coordinator.state {
        case .idle:
            HStack {
                Button("学習を開始") { coordinator.start(basePath: basePath) }
                    .disabled(!canStart)
                if let info = coordinator.info, info.usableEntries < info.minimumRecords {
                    Text("記録が \(info.minimumRecords) 件以上必要です")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
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
            guard let progress else { return "いまのモデルの出来を確認中…" }
            return "いまのモデルの出来を確認中… \(progress.done)/\(progress.total) 件"
        case "evaluate": return "学習後の変換を確認中…"
        case "quantize": return "ベースモデルを学習用に変換中…"
        case "load": return "モデルを読み込み中…"
        case "export": return "アダプタを書き出し中…"
        default:
            guard let step else { return "学習中…" }
            var text = "学習中 \(step.step)/\(step.steps)"
            if step.epochs > 0 { text += "（エポック \(step.epoch)/\(step.epochs)）" }
            text += String(format: "  損失 %.3f", step.loss)
            return text
        }
    }

    @ViewBuilder private var adapterField: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("使用する LoRA アダプタ（GGUF）のパス")
            TextField("", text: $adapterPath, prompt: Text("なし（ベースモデルのまま）"))
                .textFieldStyle(.roundedBorder)
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

/// 学習結果: 「直した変換が当たるようになったか」と「元から正しかった変換が壊れていないか」を並べて出す。
/// 体感では悪化の方が目立つので、効果だけでなく副作用も必ず見せる
private struct TrainingResultView: View {
    let result: TrainingResult
    @Binding var adapterPath: String
    let onClose: () -> Void

    private var mistakesDelta: Int { result.after.mistakes.exact - result.before.mistakes.exact }
    private var correctDelta: Int { result.after.correct.exact - result.before.correct.exact }

    /// 見つかった間違いの総数（学習に使った分 ＋ 効果測定に取り分けた分）
    private var foundMistakes: Int { result.data.mistakes + result.data.heldOutMistakes }
    /// 間違い1件を訓練データに入れた回数（`TrainingConfig.mistakeWeight`。行数から逆算する）
    private var repeatsPerMistake: Int {
        guard result.data.mistakes > 0 else { return 0 }
        return max(1, (result.data.trainLines - result.data.anchors) / result.data.mistakes)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("学習が終わりました").font(.headline)
            // 数の関係が読んで分かるように「見つけた → 学習に使った → 水増しした」の順に並べる
            VStack(alignment: .leading, spacing: 2) {
                Text("変換記録 \(result.data.screened) 件をいまのモデルで変換し直し、"
                    + "間違いを \(foundMistakes) 件見つけました。")
                if result.data.heldOutMistakes > 0 {
                    Text("うち \(result.data.mistakes) 件を覚えさせ、残る \(result.data.heldOutMistakes) 件は"
                        + "効果を測るために学習から外しました（下の「間違えていた変換」）。")
                } else {
                    Text("この \(result.data.mistakes) 件を覚えさせました。")
                }
                Text("覚えさせる \(result.data.mistakes) 件は数が少ないので \(repeatsPerMistake) 回ずつ繰り返し"
                    + "（\(result.data.trainLines - result.data.anchors) 行）、"
                    + "元から正しく変換できていた記録 \(result.data.anchors) 件を足した"
                    + "\(result.data.trainLines) 行で学習しました"
                    + "（正しく変換できていた記録も入れるのは、自分の文脈と言葉づかいをまとめて"
                    + "覚えさせるためです。できていた変換が崩れるのも防げます）。")
            }
            .font(.caption)
            .foregroundStyle(.secondary)
            if result.after.mistakes.total > 0 {
                scoreRow(title: "間違えていた変換", score: result.after.mistakes,
                         before: result.before.mistakes.exact, delta: mistakesDelta)
            }
            if result.after.correct.total > 0 {
                scoreRow(title: "正しかった変換", score: result.after.correct,
                         before: result.before.correct.exact, delta: correctDelta)
            }
            if result.after.mistakes.total > 0, result.after.mistakes.total < 10 {
                Text("評価に回せた間違いが \(result.after.mistakes.total) 件と少ないので、この数値はぶれます。"
                    + "記録が溜まるほど確かな目安になります。")
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

    /// 「N/M 件（学習前 K 件 → +2）」の 1 行
    @ViewBuilder private func scoreRow(title: String, score: TrainingScore, before: Int, delta: Int) -> some View {
        LabeledContent(title) {
            HStack(spacing: 6) {
                Text("\(score.exact)/\(score.total) 件")
                Text(delta == 0 ? "変化なし" : (delta > 0 ? "+\(delta)" : "\(delta)"))
                    .foregroundStyle(delta == 0 ? .secondary : (delta > 0 ? Color.green : Color.orange))
                Text("（学習前 \(before) 件）").foregroundStyle(.secondary)
            }
            .font(.callout)
        }
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

/// データの保存場所（設定 > 情報）。iCloud Drive / Dropbox のフォルダを指定して他のMacと共有する
private struct DataDirectorySection: View {
    @State private var pendingURL: URL?
    @State private var errorMessage: String?

    var body: some View {
        Section("データの保存場所") {
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
            Text("ユーザ辞書・変換ルール・学習・変換記録・変換モデル・設定をこのフォルダに保存します。"
                 + "iCloud DriveやDropboxのフォルダを指定すると、複数のMacで同じデータを共有できます。"
                 + "変更するとirohaが再起動します。")
                .font(.caption)
                .foregroundStyle(.secondary)
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
            }

            Section("アンインストール") {
                Button("irohaをアンインストール...", role: .destructive) {
                    showingUninstallConfirm = true
                }
                Text("入力ソースの一覧からirohaを外し、アプリ本体を削除して終了します。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
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
