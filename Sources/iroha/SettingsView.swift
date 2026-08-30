import SwiftUI
import IrohaCore

/// 設定ウィンドウの一時的な表示状態（メニューから直接ユーザ辞書を開く等）。
/// ウィンドウの生成タイミングに依存しないよう、プロセスで1つを共有する
@MainActor
final class SettingsUIState: ObservableObject {
    static let shared = SettingsUIState()
    @Published var showingUserDictionary = false
}

/// irohaの設定ウィンドウ。値はUserDefaults（irohaのドメイン）に保存され、
/// コントローラ側が都度読み出す。
struct SettingsView: View {
    @AppStorage("liveConversion") private var liveConversion = true
    @AppStorage("commitOnPunctuation") private var commitOnPunctuation = true
    @AppStorage("punctuationStyle") private var punctuationStyle = "、。"
    @AppStorage("candidateCount") private var candidateCount = 8
    @AppStorage("modelPath") private var modelPath = ""
    @AppStorage("translateCommitModifier") private var translateCommitModifier = "control"
    @AppStorage(TranslationBackend.userDefaultsKey) private var translationService = "apple"
    @AppStorage("ollamaModel") private var ollamaModel = ""
    @AppStorage("lmStudioModel") private var lmStudioModel = ""
    @AppStorage("autoUpdateCheck") private var autoUpdateCheck = true
    @AppStorage(UserDictionarySync.autoSyncKey) private var syncSystemDictionary = false
    @AppStorage(LearningSettings.enabledKey) private var learningEnabled = true
    @ObservedObject private var modelDownloader = ModelDownloader.shared

    @State private var remoteModels: [String] = []
    @State private var remoteModelsError: String?
    @State private var loadingRemoteModels = false
    @ObservedObject private var uiState = SettingsUIState.shared
    @State private var userDictionaryCount = UserDictionaryStore.shared.entries.count
    @State private var learningCount = LearningStore.shared.count

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

            Section("英訳して確定") {
                Picker("ショートカット", selection: $translateCommitModifier) {
                    Text("オフ").tag("off")
                    Text("⌃ control + Return").tag("control")
                    Text("⌥ option + Return").tag("option")
                    Text("⇧ shift + Return").tag("shift")
                }
                Picker("翻訳サービス", selection: $translationService) {
                    Text("Apple Intelligence（オンデバイス）").tag("apple")
                    Text("Ollama").tag("ollama")
                    Text("LM Studio").tag("lmstudio")
                }
                if translationService != "apple" {
                    Picker("モデル", selection: remoteModelBinding) {
                        Text("選択してください").tag("")
                        ForEach(remoteModelChoices, id: \.self) { model in
                            Text(model).tag(model)
                        }
                    }
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
                TextField("モデルファイル（GGUF）のパス", text: $modelPath,
                          prompt: Text(ZenzEngine.defaultModelPath))
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

            Section {
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
        .frame(width: 440)
        .fixedSize(horizontal: false, vertical: true)
        .sheet(isPresented: $uiState.showingUserDictionary) { UserDictionaryView() }
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

    // MARK: - 翻訳サービス（Ollama / LM Studio）

    private var remoteModelBinding: Binding<String> {
        translationService == "ollama" ? $ollamaModel : $lmStudioModel
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
        switch translationService {
        case "ollama":
            return "ローカルのOllama（\(RemoteTranslator.ollamaEndpoint)）で英訳します。"
                + "thinking（思考過程）は無効化されます。失敗時は通常の確定になります。"
        case "lmstudio":
            return "ローカルのLM Studio（\(RemoteTranslator.lmStudioEndpoint)）で英訳します。"
                + "thinking（思考過程）は出力されません。失敗時は通常の確定になります。"
        default:
            return TranslationService.appleAvailable
                ? "未確定文字列をオンデバイスAI（Apple Intelligence）で英訳して入力します。"
                : "Apple Intelligence（macOS 26以降で有効化）が必要です。利用できない間は通常の確定になります。"
        }
    }

    private func fetchRemoteModels() {
        guard translationService != "apple" else { return }
        let service: RemoteTranslator.Service = translationService == "ollama" ? .ollama : .lmstudio
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
