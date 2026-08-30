import SwiftUI
import IrohaCore

/// irohaの設定ウィンドウ。値はUserDefaults（irohaのドメイン）に保存され、
/// コントローラ側が都度読み出す。
struct SettingsView: View {
    @AppStorage("liveConversion") private var liveConversion = true
    @AppStorage("commitOnPunctuation") private var commitOnPunctuation = true
    @AppStorage("punctuationStyle") private var punctuationStyle = "、。"
    @AppStorage("candidateCount") private var candidateCount = 8
    @AppStorage("modelPath") private var modelPath = ""
    @AppStorage("translateCommitModifier") private var translateCommitModifier = "control"
    @AppStorage("autoUpdateCheck") private var autoUpdateCheck = true
    @ObservedObject private var modelDownloader = ModelDownloader.shared

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
                Text(TranslationService.isAvailable
                     ? "未確定文字列をオンデバイスAI（Apple Intelligence）で英訳して入力します。"
                     : "Apple Intelligence（macOS 26以降で有効化）が必要です。利用できない間は通常の確定になります。")
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
                    // プロセスを終了すると、次の入力時にシステムが自動で再起動する。
                    // NSApp.terminate → exit() 経由だと、C++静的デストラクタで
                    // ggml(llama.cpp)のMetal解放処理がabortしてSIGABRTになり、
                    // IMEの入力接続が壊れることがある。_exitで終了処理をスキップする
                    // （モデルやKVキャッシュはプロセス内メモリのみで、失って問題ない）
                    UserDefaults.standard.synchronize()
                    _exit(0)
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
                    Text("\(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?") (zenz-v3.1 / llama.cpp)")
                }
                Text("変換モデル zenz-v3.1（Keita Miwa氏, CC-BY-SA-4.0）/ llama.cpp（MIT）")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .frame(width: 440)
        .fixedSize(horizontal: false, vertical: true)
    }
}
