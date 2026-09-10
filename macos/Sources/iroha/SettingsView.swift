import ApplicationServices
import SwiftUI
import IrohaCore

/// 設定ウィンドウのタブ
enum SettingsTab: Hashable {
    case input       // 入力・変換のふるまい
    case dictionary  // ユーザ辞書と学習
    case ai          // AI変換（入力中のAI確定 + 選択テキストのAI編集）
    case model       // モデル（かな漢字変換モデル + AIサービス）
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

    /// メニューの「ユーザ辞書...」から呼ぶ: 辞書タブを開いて編集シートを出す
    func openUserDictionary() {
        selectedTab = .dictionary
        showingUserDictionary = true
    }

    /// メニューの「変換ルール...」から呼ぶ: 辞書タブを開いてルール編集シートを出す
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
                .tabItem { Label("辞書", systemImage: "character.book.closed") }
                .tag(SettingsTab.dictionary)
            AISettingsTab()
                .tabItem { Label("AI変換", systemImage: "sparkles") }
                .tag(SettingsTab.ai)
            ModelSettingsTab()
                .tabItem { Label("モデル", systemImage: "cube") }
                .tag(SettingsTab.model)
            AboutSettingsTab()
                .tabItem { Label("情報", systemImage: "info.circle") }
                .tag(SettingsTab.about)
        }
        // タブごとに高さが変わらないよう固定サイズにする（収まらない分はフォーム内でスクロール）。
        // macOS 26ではタブがタイトルバーに入るため、全項目が折り畳まれない幅が要る
        .frame(minWidth: 600, idealWidth: 600, minHeight: 690, idealHeight: 690)
        .sheet(isPresented: $uiState.showingUserDictionary) { UserDictionaryView() }
        .sheet(isPresented: $uiState.showingRewriteRules) { UserRewriteRulesView() }
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
    @AppStorage(UserDictionarySync.autoSyncKey) private var syncSystemDictionary = false
    @ObservedObject private var uiState = SettingsUIState.shared

    @State private var userDictionaryCount = UserDictionaryStore.shared.entries.count
    @State private var rewriteRuleCount = UserRewriteRuleStore.shared.rules.count
    @State private var learningCount = LearningStore.shared.count

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

            Section("学習") {
                Toggle("変換の修正を学習する", isOn: $learningEnabled)
                LabeledContent("学習した変換") {
                    HStack {
                        Text("\(learningCount) 件").foregroundStyle(.secondary)
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
                Text("文節変換（スペースキー）で候補を選び直して確定すると、その変換を覚えて"
                    + "次から最初に出します。同じ読みでも文中の位置で使い分けます"
                    + "（「きしゃのきしゃ」を「記者の貴社」に直すと、次からそう変換されます）。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
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
}

// MARK: - AI変換（入力中のAI確定 + 選択テキストのAI編集）

private struct AISettingsTab: View {
    // 選択テキストのAI編集
    @AppStorage(SelectionSettings.enabledKey) private var selectionEnabled = false
    @AppStorage(SelectionSettings.triggerModeKey) private var triggerMode = "bubble"
    @AppStorage(SelectionSettings.onDemandHotkeyKey) private var onDemandHotkey = "Ctrl+0"
    @AppStorage(SelectionSettings.excludedBundleIdsKey) private var excludedBundleIds = ""

    var body: some View {
        Form {
            // グループ1: 入力中の未確定文字列を修飾キー+ReturnでAI変換して確定。
            // グループ全体を1つのSectionにまとめ、1枚のカードとして描画する
            Section {
                Text("修飾キー+Returnで、入力中の未確定文字列をAIに渡し、返ってきた結果を確定します。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                AICommitPresetEditor(index: 0)
                AICommitPresetEditor(index: 1)
                AICommitPresetEditor(index: 2)
            } header: {
                Label("AI変換して確定（入力中）", systemImage: "return")
                    .font(.headline)
            }
            .headerProminence(.increased)

            // グループ2: 画面上の選択テキストをAIで書き換える。こちらも1枚のカード
            Section {
                SelectionIntroRows(selectionEnabled: $selectionEnabled)

                FormSubheader("マウスで選択したとき")
                Picker("トリガー", selection: $triggerMode) {
                    ForEach(SelectionTriggerMode.allCases) { mode in
                        Text(mode.label).tag(mode.rawValue)
                    }
                }
                .disabled(!selectionEnabled)

                FormSubheader("その場でAIに指示")
                HotkeyField(label: "ショートカット", hotkey: $onDemandHotkey)
                    .disabled(!selectionEnabled)
                Text("選択テキストに自由な指示を出せます（選択なしで押すとテキスト生成になります）。")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Text("プリセットのショートカットは、テキストを選択していないときに押すと"
                    + "「テキストを生成」の入力欄になり、結果をカーソル位置へ挿入します。")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                SelectionPresetEditor(index: 0)
                SelectionPresetEditor(index: 1)
                SelectionPresetEditor(index: 2)
                SelectionPresetEditor(index: 3)
                SelectionPresetEditor(index: 4)

                FormSubheader("除外するアプリ")
                TextField(
                    "", text: $excludedBundleIds,
                    prompt: Text("com.example.app, com.example.other"))
                    .textFieldStyle(.roundedBorder)
                    .disabled(!selectionEnabled)
                Text("ここに書いたバンドルIDのアプリでは、マウス選択のトリガーを出しません"
                    + "（カンマまたは改行区切り）。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Label("選択テキストのAI編集", systemImage: "cursorarrow.rays")
                    .font(.headline)
                    .padding(.top, 24)
            }
            .headerProminence(.increased)
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

/// AIサービス（バックエンド）の設定セクション。「モデル」タブに置く
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
            Text("ユーザ辞書・学習・変換ルール・変換モデル・設定をこのフォルダに保存します。"
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
                LabeledContent("変換モデル") {
                    Text(IrohaInputController.engineModelDisplayName)
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
                + "終了します。「データも含めて削除」を選ぶと、ユーザ辞書・学習・変換モデル・"
                + "設定・APIキーも削除します。")
        }
    }
}
