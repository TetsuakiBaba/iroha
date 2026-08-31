import ApplicationServices
import SwiftUI
import IrohaCore

/// 設定ウィンドウのタブ
enum SettingsTab: Hashable {
    case input       // 入力・変換のふるまい
    case dictionary  // ユーザ辞書と学習
    case selection   // 選択テキストのAI処理（グローバルショートカット）
    case ai          // AIで処理して確定
    case model       // かな漢字変換モデル
    case about       // アップデートとバージョン情報
}

/// 設定ウィンドウの一時的な表示状態（メニューから直接ユーザ辞書を開く等）。
/// ウィンドウの生成タイミングに依存しないよう、プロセスで1つを共有する
@MainActor
final class SettingsUIState: ObservableObject {
    static let shared = SettingsUIState()
    @Published var selectedTab: SettingsTab = .input
    @Published var showingUserDictionary = false

    /// メニューの「ユーザ辞書...」から呼ぶ: 辞書タブを開いて編集シートを出す
    func openUserDictionary() {
        selectedTab = .dictionary
        showingUserDictionary = true
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
            SelectionSettingsTab()
                .tabItem { Label("選択", systemImage: "cursorarrow.rays") }
                .tag(SettingsTab.selection)
            AISettingsTab()
                .tabItem { Label("AI", systemImage: "sparkles") }
                .tag(SettingsTab.ai)
            ModelSettingsTab()
                .tabItem { Label("モデル", systemImage: "cube") }
                .tag(SettingsTab.model)
            AboutSettingsTab()
                .tabItem { Label("情報", systemImage: "info.circle") }
                .tag(SettingsTab.about)
        }
        // タブごとに高さが変わらないよう固定サイズにする（収まらない分はフォーム内でスクロール）。
        // macOS 26ではタブがタイトルバーに入るため、6項目が折り畳まれない幅が要る
        .frame(minWidth: 600, idealWidth: 600, minHeight: 690, idealHeight: 690)
        .sheet(isPresented: $uiState.showingUserDictionary) { UserDictionaryView() }
    }
}

// MARK: - 入力

private struct InputSettingsTab: View {
    @AppStorage("liveConversion") private var liveConversion = true
    @AppStorage("commitOnPunctuation") private var commitOnPunctuation = true
    @AppStorage("candidateCount") private var candidateCount = 8
    @AppStorage("punctuationStyle") private var punctuationStyle = "、。"

    var body: some View {
        Form {
            Section("変換") {
                Toggle("ライブ変換", isOn: $liveConversion)
                Toggle("句読点で自動確定", isOn: $commitOnPunctuation)
                    .disabled(!liveConversion)
                Stepper(value: $candidateCount, in: 3...16) {
                    HStack {
                        Text("候補ウィンドウの候補数")
                        Spacer()
                        Text("\(candidateCount)").foregroundStyle(.secondary)
                    }
                }
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

// MARK: - 辞書・学習

private struct DictionarySettingsTab: View {
    @AppStorage(LearningSettings.enabledKey) private var learningEnabled = true
    @AppStorage(UserDictionarySync.autoSyncKey) private var syncSystemDictionary = false
    @ObservedObject private var uiState = SettingsUIState.shared

    @State private var userDictionaryCount = UserDictionaryStore.shared.entries.count
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

            Section("学習") {
                Toggle("変換の修正を学習する", isOn: $learningEnabled)
                LabeledContent("学習した変換") {
                    HStack {
                        Text("\(learningCount) 件").foregroundStyle(.secondary)
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
            NotificationCenter.default.publisher(for: LearningStore.didChangeNotification)
        ) { _ in
            learningCount = LearningStore.shared.count
        }
    }
}

// MARK: - AI

private struct AISettingsTab: View {
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
        Form {
            Section {
                Text("修飾キー+Returnで、未確定文字列をAIに渡し、返ってきた結果を確定します。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("AI変換して確定")
            }

            ForEach(0..<AICommitSettings.count, id: \.self) { index in
                AICommitPresetEditor(index: index)
            }

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
        }
        .formStyle(.grouped)
        .onAppear { fetchRemoteModels() }
    }

    // MARK: AIサービス（Apple Intelligence / Ollama / LM Studio）

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
        let common = "AI変換と選択テキスト処理のすべてがこのサービスを使います。"
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
                + "thinking（思考過程）は出力されません。失敗時は通常の確定になります。"
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
        Section {
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
        } header: {
            Text("\(index + 1). \(headerName)")
        }
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


// MARK: - 選択テキストのAI処理

private struct SelectionSettingsTab: View {
    @AppStorage(SelectionSettings.enabledKey) private var enabled = false
    @AppStorage(SelectionSettings.triggerModeKey) private var triggerMode = "bubble"
    @AppStorage(SelectionSettings.onDemandHotkeyKey) private var onDemandHotkey = "Ctrl+0"
    @AppStorage(SelectionSettings.excludedBundleIdsKey) private var excludedBundleIds = ""

    @State private var accessibilityGranted = AXIsProcessTrusted()

    var body: some View {
        Form {
            Section {
                Toggle("選択テキストのAI処理を有効にする", isOn: $enabled)
                Text("どのアプリでも、選択したテキストをショートカットやマウス操作からAIで"
                    + "処理して置き換えられます。処理には設定 > AI のAIサービスを使います。"
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
            } header: {
                Text("選択テキストのAI処理")
            }

            Section("マウスで選択したとき") {
                Picker("トリガー", selection: $triggerMode) {
                    ForEach(SelectionTriggerMode.allCases) { mode in
                        Text(mode.label).tag(mode.rawValue)
                    }
                }
                .disabled(!enabled)
            }

            Section("その場でAIに指示") {
                HotkeyField(label: "ショートカット", hotkey: $onDemandHotkey)
                    .disabled(!enabled)
                Text("選択テキストに自由な指示を出せます（選択なしで押すとテキスト生成になります）。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            ForEach(0..<SelectionSettings.count, id: \.self) { index in
                SelectionPresetEditor(index: index)
                    .disabled(!enabled)
            }

            Section("除外するアプリ") {
                TextField(
                    "", text: $excludedBundleIds,
                    prompt: Text("com.example.app, com.example.other"))
                    .textFieldStyle(.roundedBorder)
                    .disabled(!enabled)
                Text("ここに書いたバンドルIDのアプリでは、マウス選択のトリガーを出しません"
                    + "（カンマまたは改行区切り）。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .onAppear { accessibilityGranted = AXIsProcessTrusted() }
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
        Section {
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
        } header: {
            Text("プリセット\(index + 1). \(headerName)")
        }
    }

    private var headerName: String {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? SelectionSettings.defaults[index].name : trimmed
    }
}

// MARK: - 変換モデル

private struct ModelSettingsTab: View {
    @AppStorage("modelPath") private var modelPath = ""
    @ObservedObject private var modelDownloader = ModelDownloader.shared

    var body: some View {
        Form {
            Section("変換モデル") {
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
                        let dir = NSHomeDirectory() + "/Library/Application Support/iroha/models"
                        try? FileManager.default.createDirectory(
                            atPath: dir, withIntermediateDirectories: true)
                        NSWorkspace.shared.open(URL(fileURLWithPath: dir))
                    }
                    Button("ファイルを選択...") {
                        let panel = NSOpenPanel()
                        panel.allowedContentTypes = []
                        panel.allowsOtherFileTypes = true
                        panel.canChooseDirectories = false
                        panel.directoryURL = URL(
                            fileURLWithPath: NSHomeDirectory()
                                + "/Library/Application Support/iroha/models")
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
        }
        .formStyle(.grouped)
    }
}

// MARK: - 情報

private struct AboutSettingsTab: View {
    @AppStorage("autoUpdateCheck") private var autoUpdateCheck = true

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
                Text("既定の変換モデル zenz-v3.1（Keita Miwa氏, CC-BY-SA-4.0）/ llama.cpp（MIT）")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }
}
