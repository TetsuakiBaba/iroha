import Cocoa
import InputMethodKit
import IrohaCore

/// キーイベントを処理するIMEコントローラ。
///
/// - ライブ変換: 入力のたびに読みをLLM（zenz-v3）へ送り、変換結果を未確定文字列として表示
/// - スペースキー: 文節変換モードへ（←→で文節移動、Shift+←→で伸縮、Spaceで候補ウィンドウ）
/// - Enterで確定、Escでかな表示に戻す/取消、BSで編集
/// - F6-F10 / Ctrl+U,I,O,P,T: ひらがな/カタカナ/半角カナ/全角英数/半角英数
/// - Shift+英字: Shiftを押している間だけ英字入力（離すとその英字を固定してかな入力に戻る）
/// - 予測変換（設定・既定OFF）: 入力の休止後に続きの文節をカーソル下の小窓に表示、Tabで取り入れる
/// - インライン補完（設定・既定OFF）: 確定の休止後に続きの文節を同じ小窓に表示、Tabで確定する
///   （どちらも `PredictionPanel`。未確定文字列には混ぜない）
@objc(IrohaInputController)
final class IrohaInputController: IMKInputController {

    /// エンジンが読み込むモデルのパス（プロセス起動時に確定。変更は再起動後に反映）。
    /// UserDefaultsの"modelPath"でモデルファイルを差し替えられる
    static let engineModelPath: String = {
        if let path = UserDefaults.standard.string(forKey: "modelPath"), !path.isEmpty {
            return path
        }
        return ZenzEngine.defaultModelPath
    }()

    /// 表示用のモデル名（ファイル名。未取得ならその旨）
    static var engineModelDisplayName: String {
        guard FileManager.default.fileExists(atPath: engineModelPath) else {
            return "モデル未取得"
        }
        return URL(fileURLWithPath: engineModelPath).deletingPathExtension().lastPathComponent
    }

    /// 変換エンジンはプロセスで1つを共有する（モデルは初回変換時にロード）。
    /// 学習 → ユーザ辞書 → 長い読みの区切り → 異体字の補完 → 辞書ラティス+zenz の順にデコレータで包む
    /// （学習・辞書が空で読みが短ければ素通しなのでふるまいは変わらない）。
    /// 辞書ラティス（azooKey辞書）は候補ウィンドウの候補を読みが正しい語に限るために使う。
    /// 辞書がバンドルに無ければzenz単体で動く
    private static let engine: any ConversionEngine = LearningEngine(
        base: UserDictionaryEngine(
            base: ChunkedConversionEngine(base: VariantKanjiEngine(base: makeCoreEngine()))),
        dictionary: { LearningSettings.dictionary })

    /// zenzモデルの実体。かな漢字変換と、同じモデルを指定した予測変換・インライン補完で共有する
    /// （モデルを二重にロードしない）
    private static let zenz = ZenzEngine(modelPath: engineModelPath)

    /// 予測変換（確定前）とインライン補完（確定後）のエンジン。かな漢字変換とは別のモデルを
    /// 設定できる（`PredictionSettings`）。既定はどちらもかな漢字変換のzenzを共有する
    private static let predictionEngine: any PredictionEngine = PredictionSettings.engine(
        forKey: PredictionSettings.predictiveModelPathKey, fallbackPath: engineModelPath,
        sharing: [(engineModelPath, zenz)])
    private static let completionEngine: any PredictionEngine = PredictionSettings.engine(
        forKey: PredictionSettings.completionModelPathKey, fallbackPath: engineModelPath,
        sharing: [
            (engineModelPath, zenz),
            (PredictionSettings.resolvedModelPath(
                forKey: PredictionSettings.predictiveModelPathKey, fallback: engineModelPath), predictionEngine),
        ])

    private static func makeCoreEngine() -> any ConversionEngine {
        guard let dictionaryURL = LatticeConverter.defaultDictionaryURL() else {
            NSLog("iroha: 辞書ラティスの辞書が見つかりません。zenz単体で変換します")
            return zenz
        }
        return LatticeRescoringEngine(base: zenz, lattice: LatticeConverter(dictionaryURL: dictionaryURL))
    }

    private enum Mode {
        case composing          // 入力・ライブ変換中
        case segmenting         // 文節変換中（スペースキー押下後）
    }

    /// 文節変換中の1文節
    private struct BunsetsuSegment {
        var reading: String         // ひらがなの読み
        var result: String          // 現在選ばれている変換結果
        var candidates: [String]?   // 取得済みの候補（キャッシュ）
        /// 候補のうち、選んで確定しても学習に記録しないもの。
        /// - ユーザ定義の変換ルール（User Rewriter）の出力: 日付・時刻のように毎回変わる
        /// - ユーザ辞書のうち候補ウィンドウにだけ出す語（ハッシュタグ・定型文など）:
        ///   学習が覚えるとライブ変換に戻ってきてしまう
        var unlearnableCandidates: Set<String> = []
    }

    private var mode: Mode = .composing
    private var composer = IrohaInputController.makeComposer()

    /// 句読点スタイル（"、。" または "，．"のセット）
    private static let punctuationStyleKey = "punctuationStyle"
    static var punctuationStyle: String {
        if let style = UserDefaults.standard.string(forKey: punctuationStyleKey),
           style == "、。" || style == "，．" {
            return style
        }
        // 旧設定（読点・句点の個別キー）からの移行
        return UserDefaults.standard.string(forKey: "commaStyle") == "，" ? "，．" : "、。"
    }

    /// 設定（句読点スタイル）を反映したRomajiComposerを作る
    private static func makeComposer() -> RomajiComposer {
        let style = Self.punctuationStyle
        return RomajiComposer(
            commaText: String(style.first ?? "、"),
            periodText: String(style.last ?? "。")
        )
    }

    /// 候補ウィンドウに出す候補数（設定、デフォルト8）
    private var candidateCount: Int {
        let value = UserDefaults.standard.integer(forKey: "candidateCount")
        return value == 0 ? 8 : max(3, min(16, value))
    }
    /// 現在の入力モード（Info.plistのtsInputModeListKeyに対応）
    private var japaneseMode = true

    // MARK: 設定（UserDefaults）

    private static let liveConversionKey = "liveConversion"
    private var liveConversionEnabled: Bool {
        UserDefaults.standard.object(forKey: Self.liveConversionKey) as? Bool ?? true
    }
    private static let commitOnPunctuationKey = "commitOnPunctuation"
    private var commitOnPunctuationEnabled: Bool {
        UserDefaults.standard.object(forKey: Self.commitOnPunctuationKey) as? Bool ?? false
    }

    // MARK: 状態

    /// F6/F7等による表示の強制上書き（ひらがな・カタカナ・英数）。
    /// 次の入力でfixedChunksへ取り込まれる
    private var displayOverride: String?

    /// F6-F10 / Ctrl+U,I,O,P,Tで表示形を指定した部分。
    /// 指定した見た目のまま未確定文字列の先頭に残し、以降の入力ではライブ変換の対象にしない
    private struct FixedChunk {
        enum Kind {
            case display         // F6-F10 / Ctrl+U…T で指定した表示形
            case liveConversion  // Shift+英字で英字入力を始めた時点のライブ変換結果で固定したかな
            case alphabet        // Shift+英字で入力した英字（readingも英字そのまま。かなには戻さない）
            case prediction      // 予測変換でTabで取り入れた文節（読みは不明。readingにはtextを入れる。かなには戻さない）
        }
        var reading: String   // 元の読み（BS・Escでかなに戻すときに使う）
        var text: String      // 固定した表示（カタカナ・英数など）
        var kind: Kind = .display
    }
    private var fixedChunks: [FixedChunk] = []
    /// Shift+英字で入力中の英字（合成の末尾に付く。nilなら英字入力中でない）。
    /// Shiftを押している間だけ続き、Shiftを離して小文字などを打つと `.alphabet` の固定部分になって
    /// かな入力に戻る（「今日はAIを使う」を1回のEnterで確定できる）。この間 composer は空
    private var alphabetRun: String?
    /// 固定部分の表示文字列
    private var fixedText: String { fixedChunks.map(\.text).joined() }
    /// 句読点入力後、変換結果の到着を待って自動確定するフラグ
    private var autoCommitPending = false
    /// 最後に完了したライブ変換の（読み, 変換結果）
    private var lastConversion: (reading: String, result: String)?
    private var conversionTask: Task<Void, Never>?
    /// 文脈条件付けに使う直前の確定文字列（最大40文字）。
    /// アプリのテキストを読めないときの文脈、および学習・補完の整合性チェックに使う
    private var recentCommitted = ""
    /// 合成を始めた時点でアプリから読んだカーソル手前のテキスト（`DocumentContextSettings`）。
    /// 設定OFF・アプリが返さないときは nil で `recentCommitted` に代える
    private var documentContext: String?
    /// 今の合成の変換・予測に渡す左文脈（未確定文字列の固定部分は呼び出し側で足す）
    private var conversionContext: String { documentContext ?? recentCommitted }

    /// 文節変換中の状態
    private var segments: [BunsetsuSegment] = []
    /// 文節変換に入った時点のエンジンの変換結果。
    /// これと違う内容で確定されたら「ユーザによる修正」とみなして学習する
    private var segmentBaseline: String?
    private var currentSegmentIndex = 0
    /// 非同期の文節処理が古い状態に適用されるのを防ぐ世代カウンタ
    private var segmentGeneration = 0
    /// 候補ウィンドウに表示中の候補（candidates(_:)が返す）
    private var panelCandidates: [String] = []
    private var panelVisible = false

    /// 英訳確定の進行中フラグと世代（Escや他経路のcommitTextで無効化する）
    private var isTranslating = false
    private var translationGeneration = 0
    private var translationTask: Task<Void, Never>?
    /// 翻訳中表示の状態（翻訳対象の日本語・ストリーミング途中の英文・スピナー）
    private var translatingJapanese = ""
    /// 実行中のプリセット名（結果が届くまでの表示に使う）
    private var translationLabel = ""
    private var translationPartial = ""
    private var translationSpinnerIndex = 0
    private var translationSpinnerTimer: Timer?
    private static let spinnerFrames = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]

    // MARK: 予測変換・インライン補完の状態

    /// 予測変換: 小窓に表示中の予測。`base` は予測したときの未確定文字列で、表示がこれと違えば無効
    private var prediction: (base: String, text: String)?
    private var predictionTask: Task<Void, Never>?
    private var predictionGeneration = 0
    /// インライン補完: 確定後に小窓に表示している続き（アプリのテキストにはまだ入っていない）
    private var pendingCompletion: String?
    private var completionTask: Task<Void, Never>?
    private var completionGeneration = 0
    /// 最後にキー入力があった時刻（入力の休止時間を測る）
    private var lastKeyEventTime = ContinuousClock.now

    private var isComposing: Bool { !composer.isEmpty || !fixedChunks.isEmpty || alphabetRun != nil }

    /// 変換をやめてかなで見せるときの表示（固定部分 + 入力中のかな）
    private var kanaDisplay: String { fixedText + composer.display + (alphabetRun ?? "") }

    private var candidatesPanel: IMKCandidates? {
        (NSApp.delegate as? AppDelegate)?.candidatesPanel
    }

    // MARK: - IMKInputController

    override func recognizedEvents(_ sender: Any!) -> Int {
        Int(NSEvent.EventTypeMask.keyDown.rawValue)
    }

    override func activateServer(_ sender: Any!) {
        super.activateServer(sender)
        // フォーカスが移ったら文脈をリセット
        recentCommitted = ""
        documentContext = nil
        cancelPrediction()
        dismissCompletion()
        // モデルを事前ロード（未ロード時のみ実処理が走る）
        Task { try? await Self.engine.prewarm() }
        // 予測変換・インライン補完に別のモデルを指定している場合はそれも（同じモデルなら何もしない）
        if PredictionSettings.isPredictiveEnabled { Task { try? await Self.predictionEngine.prewarm() } }
        if PredictionSettings.isCompletionEnabled { Task { try? await Self.completionEngine.prewarm() } }
        Task.detached { TranslationService.prewarm() }
        // アップデートの自動確認（1日1回まで、設定でOFF可）
        Task { await UpdateChecker.shared.autoCheckIfDue() }
        // 変換モデルが未取得のままなら再試行（起動時にオフラインだった場合など。60秒スロットル付き）
        ModelDownloader.shared.startIfNeeded()
    }

    override func setValue(_ value: Any!, forTag tag: Int, client sender: Any!) {
        // 入力モード切替（ひらがな⇔英数）の通知
        if let mode = value as? String, mode.hasPrefix("com.apple.inputmethod") {
            let newJapaneseMode = mode.contains("Japanese")
            if japaneseMode != newJapaneseMode {
                commitCurrent(client: sender as? IMKTextInput, suggestsCompletion: false)
                japaneseMode = newJapaneseMode
            }
        }
        super.setValue(value, forTag: tag, client: sender)
    }

    override func handle(_ event: NSEvent!, client sender: Any!) -> Bool {
        guard let event, event.type == .keyDown,
              let client = sender as? IMKTextInput else { return false }

        lastKeyEventTime = .now
        let plainTab = Int(event.keyCode) == kVK_Tab
            && event.modifierFlags.intersection([.command, .control, .option, .shift]).isEmpty

        // インライン補完（確定後の予測の小窓）: Tabで確定、それ以外のキーではまず閉じてから通常処理へ
        if pendingCompletion != nil {
            if plainTab {
                acceptCompletion(client: client)
                return true
            }
            dismissCompletion()
            // Escは小窓を閉じるだけで飲み込む（アプリのダイアログ等を閉じてしまわないように）
            if Int(event.keyCode) == kVK_Escape { return true }
        } else {
            completionTask?.cancel()
            completionTask = nil
        }

        // 予測変換（入力中の予測の小窓）: Tabで取り入れる。それ以外のキーでは閉じる
        if prediction != nil, plainTab, mode == .composing, acceptPrediction(client: client) {
            return true
        }
        cancelPrediction()

        // 選択テキスト処理のグローバルショートカットが未確定文字列と競合しないよう、
        // キー処理後の合成状態を知らせておく（IMKはメインスレッドで呼ぶ）
        defer {
            SelectionActionCoordinator.isIMEComposing =
                isComposing || mode == .segmenting || isTranslating
        }

        // 英訳確定の待機中: Escで取消、それ以外は握りつぶす（最大でもタイムアウトの10秒）
        if isTranslating {
            if Int(event.keyCode) == kVK_Escape {
                cancelTranslation(client: client)
            }
            return true
        }

        // 英数/かなキー（JISキーボード）はモードに関わらず処理する
        switch Int(event.keyCode) {
        case kVK_JIS_Eisu:
            if japaneseMode {
                commitCurrent(client: client, suggestsCompletion: false)
                client.selectMode("com.apple.inputmethod.Roman")
            }
            return true
        case kVK_JIS_Kana:
            if !japaneseMode {
                client.selectMode("com.apple.inputmethod.Japanese")
            }
            return true
        default:
            break
        }

        // Control+. : 句読点スタイル（、。⇄，．）の切り替え（モードに関わらず有効）
        if event.modifierFlags.contains(.control), !event.modifierFlags.contains(.command),
           !event.modifierFlags.contains(.option),
           event.charactersIgnoringModifiers == "." {
            togglePunctuationStyle(nil)
            return true
        }

        guard japaneseMode else { return false }

        // Windows IME互換ショートカット（Ctrl+U/I/O/P/T = ひらがな/カタカナ/半角カナ/全角英数/半角英数）
        if event.modifierFlags.contains(.control), !event.modifierFlags.contains(.command),
           isComposing, mode == .composing {
            switch event.charactersIgnoringModifiers {
            case "u": return applyFunctionKeyConversion(keyCode: kVK_F6, client: client)
            case "i": return applyFunctionKeyConversion(keyCode: kVK_F7, client: client)
            case "o": return applyFunctionKeyConversion(keyCode: kVK_F8, client: client)
            case "p": return applyFunctionKeyConversion(keyCode: kVK_F9, client: client)
            case "t": return applyFunctionKeyConversion(keyCode: kVK_F10, client: client)
            default: break
            }
        }

        // 修飾キー+Enter: 未確定文字列をAIで変換して確定（プリセットは設定で変更可能）
        if Int(event.keyCode) == kVK_Return || Int(event.keyCode) == kVK_ANSI_KeypadEnter,
           isComposing || mode == .segmenting,
           let preset = AICommitSettings.preset(
            matching: event.modifierFlags.intersection([.command, .control, .option, .shift])) {
            return handleAICommit(preset, client: client)
        }

        // Command/Control付きのキーはIMEでは扱わない（未確定文字列は確定して逃がす）
        if event.modifierFlags.contains(.command) || event.modifierFlags.contains(.control) {
            if isComposing { commitCurrent(client: client, suggestsCompletion: false) }
            return false
        }

        switch mode {
        case .segmenting:
            return handleSegmenting(event, client: client)
        case .composing:
            return handleComposing(event, client: client)
        }
    }

    override func deactivateServer(_ sender: Any!) {
        commitCurrent(client: sender as? IMKTextInput, suggestsCompletion: false)
        super.deactivateServer(sender)
    }

    override func commitComposition(_ sender: Any!) {
        // クリックやフォーカス移動による確定。小窓の予測・補完は確定に含めない
        commitCurrent(client: sender as? IMKTextInput, suggestsCompletion: false)
    }

    // MARK: - 入力メニュー（メニューバーの入力ソースアイコンから開く）

    override func menu() -> NSMenu! {
        let menu = NSMenu(title: "iroha")

        let settingsItem = NSMenuItem(
            title: "設定...",
            action: #selector(openSettings(_:)),
            keyEquivalent: ""
        )
        settingsItem.target = self
        menu.addItem(settingsItem)

        let dictionaryItem = NSMenuItem(
            title: "ユーザ辞書...",
            action: #selector(openUserDictionary(_:)),
            keyEquivalent: ""
        )
        dictionaryItem.target = self
        menu.addItem(dictionaryItem)

        let rewriteRulesItem = NSMenuItem(
            title: "変換ルール...",
            action: #selector(openRewriteRules(_:)),
            keyEquivalent: ""
        )
        rewriteRulesItem.target = self
        menu.addItem(rewriteRulesItem)

        menu.addItem(NSMenuItem.separator())

        let liveItem = NSMenuItem(
            title: "ライブ変換",
            action: #selector(toggleLiveConversion(_:)),
            keyEquivalent: ""
        )
        liveItem.target = self
        liveItem.state = liveConversionEnabled ? .on : .off
        menu.addItem(liveItem)

        let predictiveItem = NSMenuItem(
            title: "予測変換（入力中）",
            action: #selector(togglePredictiveConversion(_:)),
            keyEquivalent: ""
        )
        predictiveItem.target = self
        predictiveItem.state = PredictionSettings.isPredictiveEnabled ? .on : .off
        menu.addItem(predictiveItem)

        let completionItem = NSMenuItem(
            title: "インライン補完（確定後）",
            action: #selector(toggleInlineCompletion(_:)),
            keyEquivalent: ""
        )
        completionItem.target = self
        completionItem.state = PredictionSettings.isCompletionEnabled ? .on : .off
        menu.addItem(completionItem)

        let styleItem = NSMenuItem(
            title: "句読点スタイル: \(Self.punctuationStyle)",
            action: #selector(togglePunctuationStyle(_:)),
            keyEquivalent: "."
        )
        styleItem.keyEquivalentModifierMask = [.control]
        styleItem.target = self
        menu.addItem(styleItem)

        menu.addItem(NSMenuItem.separator())

        let modelItem = NSMenuItem(
            title: "モデルフォルダを開く",
            action: #selector(openModelFolder(_:)),
            keyEquivalent: ""
        )
        modelItem.target = self
        menu.addItem(modelItem)

        let updateItem = NSMenuItem(
            title: "アップデートを確認...",
            action: #selector(checkForUpdates(_:)),
            keyEquivalent: ""
        )
        updateItem.target = self
        menu.addItem(updateItem)

        menu.addItem(NSMenuItem.separator())
        // 変換モデルの取得状況（未取得・ダウンロード中・失敗のときだけ表示）
        if let status = ModelDownloader.shared.statusMenuText {
            let statusItem = NSMenuItem(title: status, action: nil, keyEquivalent: "")
            statusItem.isEnabled = false
            menu.addItem(statusItem)
        }
        let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
        let versionItem = NSMenuItem(
            title: "iroha \(version) (\(Self.engineModelDisplayName))",
            action: nil, keyEquivalent: "")
        versionItem.isEnabled = false
        menu.addItem(versionItem)
        return menu
    }

    @objc private func toggleLiveConversion(_ sender: Any?) {
        UserDefaults.standard.set(!liveConversionEnabled, forKey: Self.liveConversionKey)
        // OFFにしたら現在の変換表示をかなに戻す
        if !liveConversionEnabled, isComposing, mode == .composing, let client = client() {
            cancelConversion()
            updateMarkedText(client: client, display: kanaDisplay)
        }
    }

    @objc private func togglePredictiveConversion(_ sender: Any?) {
        UserDefaults.standard.set(!PredictionSettings.isPredictiveEnabled,
                                  forKey: PredictionSettings.predictiveEnabledKey)
        if !PredictionSettings.isPredictiveEnabled { cancelPrediction() }
    }

    @objc private func toggleInlineCompletion(_ sender: Any?) {
        UserDefaults.standard.set(!PredictionSettings.isCompletionEnabled,
                                  forKey: PredictionSettings.completionEnabledKey)
        if !PredictionSettings.isCompletionEnabled { dismissCompletion() }
    }

    @objc private func openSettings(_ sender: Any?) {
        (NSApp.delegate as? AppDelegate)?.openSettingsWindow()
    }

    // メニュー操作はメインスレッドから来る（SettingsUIStateは@MainActor）
    @MainActor
    @objc private func openUserDictionary(_ sender: Any?) {
        SettingsUIState.shared.openUserDictionary()
        (NSApp.delegate as? AppDelegate)?.openSettingsWindow()
    }

    @MainActor
    @objc private func openRewriteRules(_ sender: Any?) {
        SettingsUIState.shared.openRewriteRules()
        (NSApp.delegate as? AppDelegate)?.openSettingsWindow()
    }

    @objc private func togglePunctuationStyle(_ sender: Any?) {
        let newStyle = Self.punctuationStyle == "、。" ? "，．" : "、。"
        UserDefaults.standard.set(newStyle, forKey: Self.punctuationStyleKey)
        // 入力中はcomposerを作り直せない（バッファが消える）ため、次の確定時に反映される
        if !isComposing, mode == .composing {
            composer = Self.makeComposer()
        }
    }

    @objc private func openModelFolder(_ sender: Any?) {
        let dir = DataDirectory.modelsURL
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        NSWorkspace.shared.open(dir)
    }

    @objc private func checkForUpdates(_ sender: Any?) {
        Task { await UpdateChecker.shared.checkAndPresent() }
    }

    // MARK: - 候補ウィンドウ（IMKCandidatesからの通知）

    override func candidates(_ sender: Any!) -> [Any]! {
        panelCandidates
    }

    override func candidateSelectionChanged(_ candidateString: NSAttributedString!) {
        guard let candidateString, mode == .segmenting,
              segments.indices.contains(currentSegmentIndex) else { return }
        segments[currentSegmentIndex].result = candidateString.string
        if let client = client() {
            refreshSegmentDisplay(client: client)
        }
    }

    override func candidateSelected(_ candidateString: NSAttributedString!) {
        guard let candidateString, mode == .segmenting,
              segments.indices.contains(currentSegmentIndex) else { return }
        segments[currentSegmentIndex].result = candidateString.string
        hidePanel()
        if let client = client() {
            refreshSegmentDisplay(client: client)
        }
    }

    // MARK: - 入力・ライブ変換中のキー処理

    private func handleComposing(_ event: NSEvent, client: IMKTextInput) -> Bool {
        switch Int(event.keyCode) {
        case kVK_Return, kVK_ANSI_KeypadEnter:
            guard isComposing else { return false }
            commitCurrent(client: client)
            return true
        case kVK_Delete:
            guard isComposing else { return false }
            if alphabetRun == nil, composer.isEmpty, fixedChunks.last?.kind == .prediction {
                // 取り入れた予測に戻ってきた: 読みが無いので1文字ずつは消せず、予測ごと取り消す。
                // 取り入れたときに固定したかなも入力中の状態に戻す（Tabの前の表示に戻る）
                displayOverride = nil
                autoCommitPending = false
                undoLastPrediction()
                updateMarkedText(client: client, display: currentDisplay)
                return true
            }
            if alphabetRun == nil, composer.isEmpty, fixedChunks.last?.kind == .alphabet {
                // 直前の英字部分に戻ってきた: 英字入力として開き直してから1文字消す
                alphabetRun = fixedChunks.removeLast().text
            }
            if alphabetRun != nil {
                displayOverride = nil
                autoCommitPending = false
                deleteAlphabetBackward(client: client)
                return true
            }
            displayOverride = nil
            autoCommitPending = false
            // 固定部分しか残っていないときは、直前の固定部分を読みに戻してから削除する
            if composer.isEmpty { unlockLastFixedChunk() }
            composer.deleteBackward()
            // 削除中はライブ変換を止めてかな表示に戻す。漢字のままだと1かな消しただけで
            // 文節構造が変わり、どこを消したのか分からなくなるため。
            // 次の1文字を入力した時点でライブ変換が再開する（そのままEnterならかなで確定）
            conversionTask?.cancel()
            conversionTask = nil
            updateMarkedText(client: client, display: kanaDisplay)
            return true
        case kVK_Escape:
            guard isComposing else { return false }
            autoCommitPending = false
            if alphabetRun != nil {
                // 英字入力を取り消して、Shiftを押す前の状態（ライブ変換表示）に戻す
                endAlphabetRun()
                updateMarkedText(client: client, display: currentDisplay)
            } else if displayOverride != nil {
                // F6/F7等の上書き表示をやめてかなに戻す
                displayOverride = nil
                updateMarkedText(client: client, display: kanaDisplay)
            } else if composer.isEmpty, fixedChunks.last?.kind == .prediction {
                // 取り入れた予測を取り消す（Backspaceと同じ。入力全体を消してしまわないように）
                undoLastPrediction()
                updateMarkedText(client: client, display: currentDisplay)
            } else if unlockFixedChunks() || lastConversion != nil {
                // 1回目のEsc: 固定した部分をかなに戻し、変換をやめてかな表示にする
                // （英字の固定部分はかなに戻せないのでそのまま残る）
                cancelConversion()
                updateMarkedText(client: client, display: kanaDisplay)
            } else {
                // 2回目のEsc: 入力自体を取り消す
                composer = Self.makeComposer()
                fixedChunks = []
                updateMarkedText(client: client, display: "")
            }
            return true
        case kVK_Space:
            guard isComposing else { return false }
            enterSegmentMode(client: client)
            return true
        case kVK_F6, kVK_F7, kVK_F8, kVK_F9, kVK_F10:
            return applyFunctionKeyConversion(keyCode: Int(event.keyCode), client: client)
        case kVK_LeftArrow, kVK_RightArrow, kVK_UpArrow, kVK_DownArrow:
            // 未確定中はカーソル移動でテキスト側に抜けないよう握りつぶす
            return isComposing
        default:
            break
        }

        // 通常の文字入力
        guard let characters = event.characters, let first = characters.first else { return false }
        guard let scalar = first.unicodeScalars.first, scalar.isASCII,
              (0x21...0x7E).contains(scalar.value) else {
            // キーはアプリに渡す（Tabでフォーカスが移ることもある）ので、確定後の補完は出さない
            if isComposing { commitCurrent(client: client, suggestsCompletion: false) }
            return false
        }
        // Shift+英字（大文字）で英字入力を始める。Shiftを押している間は記号・数字もそのまま英字に足し、
        // Shiftを離して打った文字からはかな入力に戻る（それまでの英字は固定部分になる）
        let shift = event.modifierFlags.contains(.shift)
        if Self.startsAlphabetRun(first) || (alphabetRun != nil && shift) {
            inputAlphabet(first, client: client)
            return true
        }
        captureDocumentContextIfStarting(client: client)
        finishAlphabetRun()
        lockDisplayOverride()
        composer.input(first)
        // 句読点で自動確定（ライブ変換時のみ）: 変換結果の到着を待って確定する
        autoCommitPending = liveConversionEnabled && commitOnPunctuationEnabled
            && composer.pending.isEmpty
            && composer.text.last.map { "、。！？，．".contains($0) } == true
        composerDidChange(client: client)
        return true
    }

    /// F6=ひらがな / F7=カタカナ / F8=半角カタカナ / F9=全角英数 / F10=半角英数
    private func applyFunctionKeyConversion(keyCode: Int, client: IMKTextInput) -> Bool {
        guard isComposing else { return false }
        // 固定部分しかない（新しく打った読みがない）ときは対象がない
        guard !composer.isEmpty else { return true }
        conversionTask?.cancel()
        autoCommitPending = false
        composer.flush()
        let kana = composer.text
        let converted: String?
        switch keyCode {
        case kVK_F6:
            converted = kana
        case kVK_F7:
            converted = hiraganaToKatakana(kana)
        case kVK_F8:
            converted = hiraganaToKatakana(kana).applyingTransform(.fullwidthToHalfwidth, reverse: false)
        case kVK_F9:
            // 全角英数は打鍵通りの文字列が必要（かな削除後は復元できない）
            converted = composer.rawIsReliable
                ? composer.raw.applyingTransform(.fullwidthToHalfwidth, reverse: true) : nil
        case kVK_F10:
            converted = composer.rawIsReliable ? composer.raw : nil
        default:
            converted = nil
        }
        if let converted {
            displayOverride = converted
            updateMarkedText(client: client, display: fixedText + converted)
        }
        return true  // 変換できない場合もキーは消費する（Fキーがアプリに漏れないように）
    }

    /// F6-F10等で指定した表示形を「固定部分」として取り込む（確定はしない）。
    /// 以降の入力では、この部分をライブ変換にかけ直さない
    private func lockDisplayOverride() {
        guard let text = displayOverride else { return }
        displayOverride = nil
        // applyFunctionKeyConversionでflush済みなので、composer.textが指定した部分の読み
        let reading = composer.text
        guard !text.isEmpty, !reading.isEmpty else { return }
        fixedChunks.append(FixedChunk(reading: reading, text: text))
        composer = Self.makeComposer()
        // 残りの読みは空になったので、進行中のライブ変換も破棄する
        cancelConversion()
    }

    /// 直前の固定部分を読みに戻してcomposerへ返す
    private func unlockLastFixedChunk() {
        guard let last = fixedChunks.popLast() else { return }
        composer.prependText(last.reading)
    }

    /// 固定部分を読みに戻してcomposerへ返す。英字の固定部分はかなに戻せない（読みに英字が混ざると
    /// エンジンの読み制約が崩れる）し、取り入れた予測は読みが無いので、
    /// 最後の英字・予測部分より後ろだけを戻す。戻したものがあればtrue
    @discardableResult
    private func unlockFixedChunks() -> Bool {
        let start = fixedChunks.lastIndex { $0.kind == .alphabet || $0.kind == .prediction }.map { $0 + 1 } ?? 0
        let unlocking = fixedChunks[start...]
        guard !unlocking.isEmpty else { return false }
        composer.prependText(unlocking.map(\.reading).joined())
        fixedChunks.removeSubrange(start...)
        return true
    }

    // MARK: - 英字入力（Shift+英字）

    /// 大文字の英字なら英字入力を始める（Shift・Caps Lockのどちらでも）
    private static func startsAlphabetRun(_ character: Character) -> Bool {
        character.isASCII && character.isLetter && character.isUppercase
    }

    /// 英字を1文字足す。英字入力中でなければ、入力中のかなをその時点の表示で固定して英字入力を始める。
    /// 文章の途中で英単語を入れるとき、いったん確定して英数モードに切り替えなくてよいようにする。
    /// Shiftを離して普通の文字を打つと `finishAlphabetRun` で英字を固定部分にし、かな入力に戻る
    private func inputAlphabet(_ character: Character, client: IMKTextInput) {
        captureDocumentContextIfStarting(client: client)
        if alphabetRun == nil {
            autoCommitPending = false
            lockDisplayOverride()
            lockComposerWithLiveConversion()
            alphabetRun = ""
        }
        alphabetRun?.append(character)
        updateMarkedText(client: client, display: currentDisplay)
    }

    /// 入力中のかなを、いま表示している内容（ライブ変換結果があればそれ、なければかな）で
    /// 固定部分にする。英字は読みにならないので、以降この部分を変換し直すことはない
    private func lockComposerWithLiveConversion() {
        conversionTask?.cancel()
        let readingBeforeFlush = composer.text
        composer.flush()
        let reading = composer.text
        guard !reading.isEmpty else {
            cancelConversion()
            return
        }
        let text: String
        if let lastConversion, lastConversion.reading == readingBeforeFlush {
            text = lastConversion.result + reading.dropFirst(readingBeforeFlush.count)
        } else if let lastConversion, readingBeforeFlush.hasPrefix(lastConversion.reading) {
            text = lastConversion.result + reading.dropFirst(lastConversion.reading.count)
        } else {
            text = reading
        }
        fixedChunks.append(FixedChunk(reading: reading, text: text, kind: .liveConversion))
        composer = Self.makeComposer()
        cancelConversion()
    }

    /// 英字入力を終えて、打った英字を固定部分にする（Shiftを離してかな入力に戻るとき）
    private func finishAlphabetRun() {
        guard let run = alphabetRun else { return }
        alphabetRun = nil
        guard !run.isEmpty else { return }
        fixedChunks.append(FixedChunk(reading: run, text: run, kind: .alphabet))
    }

    /// 英字入力の末尾1文字を削除する。空になったら英字入力をやめて元のかな入力に戻る
    private func deleteAlphabetBackward(client: IMKTextInput) {
        guard var run = alphabetRun else { return }
        if !run.isEmpty { run.removeLast() }
        if run.isEmpty {
            endAlphabetRun()
        } else {
            alphabetRun = run
        }
        updateMarkedText(client: client, display: currentDisplay)
    }

    /// 英字入力をやめる。英字入力を始めたときに固定したかながあれば、
    /// そのライブ変換結果ごと元に戻す（Shiftを押す前の表示に戻る）
    private func endAlphabetRun() {
        alphabetRun = nil
        guard let last = fixedChunks.last, last.kind == .liveConversion else { return }
        fixedChunks.removeLast()
        composer.prependText(last.reading)
        lastConversion = (last.reading, last.text)
    }

    /// 英字入力の候補: 打鍵通り / 小文字 / 大文字 / 先頭だけ大文字 / 全角
    private static func alphabetCandidates(_ text: String) -> [String] {
        var variants = [text, text.lowercased(), text.uppercased()]
        if let first = text.first {
            variants.append(String(first).uppercased() + text.dropFirst().lowercased())
        }
        if let fullwidth = text.applyingTransform(.fullwidthToHalfwidth, reverse: true) {
            variants.append(fullwidth)
        }
        var results: [String] = []
        for variant in variants where !results.contains(variant) { results.append(variant) }
        return results
    }

    // MARK: - 文節変換モード

    /// スペース押下: 全体を変換し、文節に分割して文節変換モードに入る
    private func enterSegmentMode(client: IMKTextInput) {
        conversionTask?.cancel()
        autoCommitPending = false
        lockDisplayOverride()
        composer.flush()
        let reading = composer.text
        // 固定部分はそのまま先頭の文節にする（変換し直さない）
        // 英字入力中ならその英字を末尾の固定文節にする（候補は大文字/小文字/全角の変種。エンジンは呼ばない）
        let alphabetSegments = alphabetRun.map {
            [BunsetsuSegment(reading: $0, result: $0, candidates: Self.alphabetCandidates($0))]
        } ?? []
        let fixedSegments = fixedChunks.map { chunk -> BunsetsuSegment in
            switch chunk.kind {
            case .alphabet:
                return BunsetsuSegment(reading: chunk.reading, result: chunk.text,
                                       candidates: Self.alphabetCandidates(chunk.text))
            case .prediction:
                // 取り入れた予測は読みが無い: 候補はそれ自身だけ、確定しても学習しない
                return BunsetsuSegment(reading: chunk.reading, result: chunk.text,
                                       candidates: [chunk.text], unlearnableCandidates: [chunk.text])
            case .display, .liveConversion:
                return BunsetsuSegment(reading: chunk.reading, result: chunk.text, candidates: nil)
            }
        } + alphabetSegments
        let alphabetSegmentIndex: Int? = alphabetSegments.isEmpty ? nil : fixedSegments.count - 1
        let fixedPrefix = fixedText
        guard !reading.isEmpty || !fixedSegments.isEmpty else { return }

        mode = .segmenting
        segmentGeneration += 1
        let generation = segmentGeneration

        // 暫定表示: 残りの読み全体を1文節として今の表示内容をそのまま使う
        let interim = (lastConversion?.reading == reading) ? lastConversion!.result : reading
        segments = fixedSegments + (reading.isEmpty ? []
            : [BunsetsuSegment(reading: reading, result: interim, candidates: nil)])
        let cachedConversion = (lastConversion?.reading == reading) ? lastConversion?.result : nil
        segmentBaseline = cachedConversion.map { fixedPrefix + $0 }
        // 英字入力からSpaceで来たときは、その英字の文節を選択して候補を出せるようにする
        currentSegmentIndex = alphabetSegmentIndex ?? min(fixedSegments.count, segments.count - 1)
        refreshSegmentDisplay(client: client)

        // 固定部分だけなら変換するものがない
        guard !reading.isEmpty else { return }

        let context = conversionContext + fixedPrefix
        conversionTask = Task { [weak self] in
            guard let self else { return }
            do {
                let full: String
                if let cachedConversion {
                    full = cachedConversion
                } else {
                    full = try await Self.engine.convert(
                        reading: reading, context: context, candidateCount: 1).first ?? reading
                }
                let aligned = ReadingAligner.segmentReading(reading, conversion: full)
                guard !Task.isCancelled else { return }
                await MainActor.run {
                    guard self.mode == .segmenting, generation == self.segmentGeneration else { return }
                    self.segmentBaseline = fixedPrefix + full
                    self.segments = fixedSegments + aligned.map {
                        BunsetsuSegment(reading: $0.reading, result: $0.conversion, candidates: nil)
                    }
                    self.currentSegmentIndex = min(fixedSegments.count, self.segments.count - 1)
                    if let client = self.client() {
                        self.refreshSegmentDisplay(client: client)
                    }
                }
            } catch is CancellationError {
            } catch {
                NSLog("iroha: 文節分割エラー: \(error)")
            }
        }
    }

    private func handleSegmenting(_ event: NSEvent, client: IMKTextInput) -> Bool {
        let shift = event.modifierFlags.contains(.shift)
        switch Int(event.keyCode) {
        case kVK_Space, kVK_DownArrow:
            if panelVisible {
                candidatesPanel?.interpretKeyEvents([Self.syntheticArrowEvent(keyCode: kVK_DownArrow)])
            } else {
                openSegmentCandidates(client: client)
            }
            return true
        case kVK_UpArrow:
            if panelVisible {
                candidatesPanel?.interpretKeyEvents([Self.syntheticArrowEvent(keyCode: kVK_UpArrow)])
            }
            return true
        case kVK_Return, kVK_ANSI_KeypadEnter:
            if panelVisible {
                // 候補を採用して選択を続ける（結果はcandidateSelectionChangedで反映済み）
                hidePanel()
                refreshSegmentDisplay(client: client)
            } else {
                commitSegments(client: client)
            }
            return true
        case kVK_Escape:
            if panelVisible {
                hidePanel()
                refreshSegmentDisplay(client: client)
            } else {
                // 文節変換をやめてかな入力状態に戻る
                exitSegmentModeToKana(client: client)
            }
            return true
        case kVK_LeftArrow:
            if shift {
                resizeCurrentSegment(by: -1, client: client)
            } else {
                hidePanel()
                moveSegmentSelection(by: -1, client: client)
            }
            return true
        case kVK_RightArrow:
            if shift {
                resizeCurrentSegment(by: +1, client: client)
            } else {
                hidePanel()
                moveSegmentSelection(by: +1, client: client)
            }
            return true
        case kVK_Delete:
            hidePanel()
            exitSegmentModeToKana(client: client)
            return true
        default:
            // 文字入力なら全文節を確定して新しい入力を始める
            if let characters = event.characters, let first = characters.first,
               let scalar = first.unicodeScalars.first, scalar.isASCII,
               (0x21...0x7E).contains(scalar.value) {
                commitSegments(client: client)
                if Self.startsAlphabetRun(first) {
                    inputAlphabet(first, client: client)
                } else {
                    captureDocumentContextIfStarting(client: client)
                    composer.input(first)
                    composerDidChange(client: client)
                }
                return true
            }
            commitSegments(client: client, suggestsCompletion: false)
            return false
        }
    }

    /// 現在の文節の候補ウィンドウを開く
    private func openSegmentCandidates(client: IMKTextInput) {
        guard segments.indices.contains(currentSegmentIndex) else { return }
        let index = currentSegmentIndex
        if let cached = segments[index].candidates {
            showPanel(with: cached)
            return
        }
        let reading = segments[index].reading
        let context = conversionContext + segments[..<index].map(\.result).joined()
        let generation = segmentGeneration
        let count = candidateCount
        Task { [weak self] in
            guard let self else { return }
            do {
                var candidates = try await Self.engine.convert(
                    reading: reading, context: context, candidateCount: count)
                // 今表示している変換結果を先頭に置く（エンジンの並びが確率順で変わっても、
                // 候補ウィンドウを開いた瞬間に表示が変わらないように）
                let current = await MainActor.run { self.segments.indices.contains(index) ? self.segments[index].result : "" }
                if !current.isEmpty {
                    candidates.removeAll { $0 == current }
                    candidates.insert(current, at: 0)
                }
                // ユーザ定義の変換ルール（User Rewriter）の出力を合流させる。
                // エンジンとは独立した候補生成源で、第一候補（今の表示）はそのままにして
                // その直後に置く（「きょう」→ スペース2回で日付が選べる）
                let rewrites = UserRewriteRuleStore.shared.current.candidates(forReading: reading)
                    .filter { !candidates.contains($0) }
                candidates.insert(contentsOf: rewrites, at: min(1, candidates.count))
                // 定番のフォールバック候補（ひらがな・カタカナ）を末尾に追加
                for extra in [reading, hiraganaToKatakana(reading)] where !candidates.contains(extra) {
                    candidates.append(extra)
                }
                // ユーザ辞書のうちライブ変換から除外した語（候補ウィンドウ専用）を含む候補も
                // 学習しない。部分一致で合成された候補（「#tag をつける」等）も対象
                let candidateOnlyWords = UserDictionaryStore.shared.current.candidateOnlyWords(in: reading)
                let unlearnable = Set(rewrites).union(candidates.filter { candidate in
                    candidateOnlyWords.contains { candidate.contains($0) }
                })
                let finalCandidates = candidates
                await MainActor.run {
                    guard self.mode == .segmenting, generation == self.segmentGeneration,
                          self.currentSegmentIndex == index else { return }
                    self.segments[index].candidates = finalCandidates
                    self.segments[index].unlearnableCandidates = unlearnable
                    self.showPanel(with: finalCandidates)
                }
            } catch {
                NSLog("iroha: 候補生成エラー: \(error)")
            }
        }
    }

    private func moveSegmentSelection(by delta: Int, client: IMKTextInput) {
        let newIndex = currentSegmentIndex + delta
        guard segments.indices.contains(newIndex) else { return }
        currentSegmentIndex = newIndex
        refreshSegmentDisplay(client: client)
    }

    /// Shift+←→: 現在の文節の読みを1文字伸縮し、現在文節と以降を再変換する
    private func resizeCurrentSegment(by delta: Int, client: IMKTextInput) {
        hidePanel()
        guard segments.indices.contains(currentSegmentIndex) else { return }
        var currentReading = segments[currentSegmentIndex].reading
        // 現在文節より後ろの読みをまとめる
        var remainderReading = segments[(currentSegmentIndex + 1)...].map(\.reading).joined()

        if delta < 0 {
            guard currentReading.count > 1 else { return }
            remainderReading = String(currentReading.removeLast()) + remainderReading
        } else {
            guard !remainderReading.isEmpty else { return }
            currentReading.append(remainderReading.removeFirst())
        }

        segmentGeneration += 1
        let generation = segmentGeneration
        let index = currentSegmentIndex
        let context = conversionContext + segments[..<index].map(\.result).joined()
        let newCurrentReading = currentReading
        let newRemainderReading = remainderReading

        // 暫定表示: 変更された部分は読みのまま見せる
        segments = Array(segments[..<index])
            + [BunsetsuSegment(reading: newCurrentReading, result: newCurrentReading, candidates: nil)]
            + (newRemainderReading.isEmpty ? []
               : [BunsetsuSegment(reading: newRemainderReading, result: newRemainderReading, candidates: nil)])
        refreshSegmentDisplay(client: client)

        conversionTask?.cancel()
        conversionTask = Task { [weak self] in
            guard let self else { return }
            do {
                let currentResult = try await Self.engine.convert(
                    reading: newCurrentReading, context: context, candidateCount: 1).first ?? newCurrentReading
                var remainderSegments: [BunsetsuSegment] = []
                if !newRemainderReading.isEmpty {
                    let remainderFull = try await Self.engine.convert(
                        reading: newRemainderReading, context: context + currentResult,
                        candidateCount: 1).first ?? newRemainderReading
                    remainderSegments = ReadingAligner.segmentReading(
                        newRemainderReading, conversion: remainderFull
                    ).map { BunsetsuSegment(reading: $0.reading, result: $0.conversion, candidates: nil) }
                }
                guard !Task.isCancelled else { return }
                let finalRemainder = remainderSegments
                await MainActor.run {
                    guard self.mode == .segmenting, generation == self.segmentGeneration else { return }
                    self.segments = Array(self.segments[..<index])
                        + [BunsetsuSegment(reading: newCurrentReading, result: currentResult, candidates: nil)]
                        + finalRemainder
                    if let client = self.client() {
                        self.refreshSegmentDisplay(client: client)
                    }
                }
            } catch is CancellationError {
            } catch {
                NSLog("iroha: 文節再変換エラー: \(error)")
            }
        }
    }

    /// 文節変換をやめて、読み（かな）の入力状態に戻る
    private func exitSegmentModeToKana(client: IMKTextInput) {
        conversionTask?.cancel()
        segmentGeneration += 1
        mode = .composing
        segments = []
        segmentBaseline = nil
        hidePanel()
        cancelConversion()
        updateMarkedText(client: client, display: kanaDisplay)
    }

    /// 全文節の変換結果を結合して確定する
    private func commitSegments(client: IMKTextInput?, suggestsCompletion: Bool = true) {
        let text = segments.map(\.result).joined()
        learnIfCorrected(committed: text)
        commitText(text.isEmpty ? kanaDisplay : text, client: client, suggestsCompletion: suggestsCompletion)
    }

    /// 文節変換の結果がエンジンの出力と違っていたら、ユーザによる修正として学習する。
    /// （修正しなかった文節も、位置ごとの文脈つきで一緒に覚える。
    /// そうしないと「きしゃのきしゃ」の後半が次回また第一候補に戻ってしまう）
    private func learnIfCorrected(committed: String) {
        guard LearningSettings.isEnabled, !segments.isEmpty, !committed.isEmpty,
              let baseline = segmentBaseline, committed != baseline else { return }
        // 変換ルールの出力（日付・時刻など）や、候補ウィンドウ専用のユーザ辞書語を選んだ確定は
        // 学習しない。覚えると「きょう → 2026/09/06」が翌日以降も第一候補になったり、
        // ライブ変換から除外したハッシュタグが学習経由でライブ変換に出てしまう
        guard !segments.contains(where: { $0.unlearnableCandidates.contains($0.result) }) else { return }
        let reading = segments.map(\.reading).joined()
        let pairs = segments.map { (reading: $0.reading, result: $0.result) }
        Task.detached(priority: .utility) {
            LearningStore.shared.record(reading: reading, result: committed, segments: pairs)
        }
    }

    /// 文節列を未確定文字列として表示する（現在の文節は太い下線）
    private func refreshSegmentDisplay(client: IMKTextInput) {
        let attributed = NSMutableAttributedString()
        var selectionLocation = 0
        for (index, segment) in segments.enumerated() {
            let underline: NSUnderlineStyle = (index == currentSegmentIndex) ? .thick : .single
            attributed.append(NSAttributedString(
                string: segment.result,
                attributes: [
                    .underlineStyle: underline.rawValue,
                    .underlineColor: NSColor.labelColor,
                ]
            ))
            if index == currentSegmentIndex {
                selectionLocation = attributed.string.utf16.count
            }
        }
        client.setMarkedText(
            attributed,
            selectionRange: NSRange(location: selectionLocation, length: 0),
            replacementRange: NSRange(location: NSNotFound, length: NSNotFound)
        )
    }

    private func showPanel(with candidates: [String]) {
        panelCandidates = candidates
        guard let panel = candidatesPanel else { return }
        panel.update()
        panel.show(kIMKLocateCandidatesBelowHint)
        panelVisible = true
    }

    private func hidePanel() {
        candidatesPanel?.hide()
        panelVisible = false
        panelCandidates = []
    }

    // MARK: - ライブ変換

    /// 入力内容が変わった: 表示を更新し、LLM変換を非同期に走らせる
    private func composerDidChange(client: IMKTextInput) {
        conversionTask?.cancel()
        updateMarkedText(client: client, display: currentDisplay)

        let reading = composer.text
        guard liveConversionEnabled, !reading.isEmpty else { return }
        // 変換済みの読みと同じなら再変換不要（表示は確定しているので予測だけ待つ）
        if lastConversion?.reading == reading {
            schedulePrediction()
            return
        }

        // 固定部分は変換し直さず、後続の変換の文脈として渡す
        let context = conversionContext + fixedText
        conversionTask = Task { [weak self] in
            guard let self else { return }
            do {
                let candidates = try await Self.engine.convert(
                    reading: reading, context: context, candidateCount: 1)
                guard !Task.isCancelled, let best = candidates.first else { return }
                await MainActor.run {
                    self.lastConversion = (reading, best)
                    // 変換中にさらに入力が進んでいたら表示しない（新しい変換の結果を待つ）
                    guard self.mode == .composing, self.composer.text == reading else { return }
                    if self.autoCommitPending {
                        // 句読点入力による自動確定
                        self.autoCommitPending = false
                        self.commitCurrent(client: self.client())
                    } else if self.displayOverride == nil, let client = self.client() {
                        self.updateMarkedText(client: client, display: self.currentDisplay)
                        // 表示が確定したので、入力の休止（残り時間）を待って続きを予測する
                        self.schedulePrediction()
                    }
                }
            } catch is CancellationError {
                // 新しい入力に置き換えられた
            } catch {
                NSLog("iroha: 変換エラー: \(error)")
            }
        }
    }

    /// 現在表示すべき未確定文字列（変換結果があればそれ、なければかな）+ 未解決ローマ字
    ///
    /// ちらつき防止: 入力が進んで読みが伸びた場合も、前回の変換結果を接頭辞として
    /// 使い続け、新しく増えたかなだけを末尾に足す（新しい変換結果が届いたら置き換わる）
    private var currentDisplay: String {
        let prefix = fixedText
        if let alphabetRun { return prefix + composer.display + alphabetRun }
        if let displayOverride { return prefix + displayOverride }
        if let lastConversion {
            if lastConversion.reading == composer.text {
                return prefix + lastConversion.result + composer.pending
            }
            if composer.text.hasPrefix(lastConversion.reading) {
                let addedKana = composer.text.dropFirst(lastConversion.reading.count)
                return prefix + lastConversion.result + addedKana + composer.pending
            }
        }
        return prefix + composer.display
    }

    private func cancelConversion() {
        conversionTask?.cancel()
        conversionTask = nil
        lastConversion = nil
    }

    // MARK: - 未確定文字列の表示と確定

    private func updateMarkedText(client: IMKTextInput, display: String) {
        let attributed = NSAttributedString(
            string: display,
            attributes: [
                .underlineStyle: NSUnderlineStyle.single.rawValue,
                .underlineColor: NSColor.labelColor,
            ]
        )
        client.setMarkedText(
            attributed,
            selectionRange: NSRange(location: display.utf16.count, length: 0),
            replacementRange: NSRange(location: NSNotFound, length: NSNotFound)
        )
    }

    /// 現在の表示内容（ライブ変換結果 or かな or 文節列）をそのまま確定する。
    /// 小窓に出している予測・補完は確定に含めない（Tabでだけ取り入れる）。
    /// `suggestsCompletion` が偽なら確定後のインライン補完を出さない（フォーカス移動・モード切替など、
    /// ユーザが文章を続ける操作ではない確定）
    private func commitCurrent(client: IMKTextInput?, suggestsCompletion: Bool = true) {
        if mode == .segmenting { hidePanel() }
        cancelPrediction()
        dismissCompletion()
        // キー入力以外の確定（フォーカス移動等）でも合成状態の変化を知らせる
        defer { SelectionActionCoordinator.isIMEComposing = false }
        guard let text = resolveCommitText() else { return }
        commitText(text, client: client, suggestsCompletion: suggestsCompletion)
    }

    /// 通常確定で挿入されるはずの文字列を解決する（composer.flushの副作用あり。
    /// 呼び出し後は必ずcommitTextするか、状態を破棄/維持したまま確定を待つこと）
    private func resolveCommitText() -> String? {
        if mode == .segmenting {
            let text = segments.map(\.result).joined()
            return text.isEmpty ? kanaDisplay : text
        }
        guard isComposing else { return nil }
        // 固定部分（F6/F7等で指定した見た目）は常にそのまま先頭に付く
        let prefix = fixedText
        if let alphabetRun {
            // 英字入力中: 固定部分 + 英字をそのまま確定する（composerは空）
            return prefix + composer.display + alphabetRun
        }
        if let displayOverride {
            // F6/F7等で上書き表示中はその内容を確定する
            return prefix + displayOverride
        }
        // 未解決ローマ字を確定（"n"→"ん"）。flushで増えたかなは変換結果の後ろに付ける
        let readingBeforeFlush = composer.text
        composer.flush()
        let flushedSuffix = String(composer.text.dropFirst(readingBeforeFlush.count))
        if let lastConversion, lastConversion.reading == readingBeforeFlush {
            return prefix + lastConversion.result + flushedSuffix
        }
        if let lastConversion, readingBeforeFlush.hasPrefix(lastConversion.reading) {
            // 変換が追いつく前の確定: 表示と同じく「変換済み接頭辞 + 追加のかな」を確定する
            let addedKana = readingBeforeFlush.dropFirst(lastConversion.reading.count)
            return prefix + lastConversion.result + addedKana + flushedSuffix
        }
        return prefix + composer.display
    }

    // MARK: - AIで処理して確定（修飾キー+Enter）

    /// 現在の未確定文字列をAI（プリセットのプロンプト）で変換して確定する。
    /// 合成状態はリセットせず生かしたまま結果を待つ（Escで通常の未確定状態に戻れる）
    private func handleAICommit(_ preset: AICommitPreset, client: IMKTextInput) -> Bool {
        if mode == .segmenting { hidePanel() }
        guard let japanese = resolveCommitText(), !japanese.isEmpty else { return true }
        guard TranslationService.isAvailable else {
            // macOS 26未満 / Apple Intelligence無効 / モデル未選択: 通常の確定にフォールバック
            commitText(japanese, client: client)
            return true
        }
        conversionTask?.cancel()  // ライブ変換の到着で翻訳中表示が上書きされないように
        autoCommitPending = false
        translationGeneration += 1
        let generation = translationGeneration
        isTranslating = true
        translatingJapanese = japanese
        translationLabel = preset.displayName
        translationPartial = ""
        translationSpinnerIndex = 0
        refreshTranslatingMarkedText(client: client)
        startTranslationSpinner()

        translationTask = Task { [weak self] in
            // ストリーミング: 届いた結果を随時マークテキストに反映する
            let request = preset.request(for: japanese)
            let output = await TranslationService.run(request, onPartial: { [weak self] partial in
                guard let self else { return }
                Task { @MainActor in
                    guard self.isTranslating,
                          generation == self.translationGeneration else { return }
                    self.translationPartial = partial
                        .trimmingCharacters(in: .whitespacesAndNewlines)
                    if let client = self.client() {
                        self.refreshTranslatingMarkedText(client: client)
                    }
                }
            })
            guard let self else { return }
            await MainActor.run {
                guard self.isTranslating,
                      generation == self.translationGeneration else { return }
                self.isTranslating = false
                // 失敗・タイムアウト時は日本語をそのまま確定（テキストを失わない）
                self.commitText(output ?? japanese, client: self.client())
            }
        }
        return true
    }

    /// 処理中の表示: 「日本語 ⇢ (途中までの結果)スピナー」をグレー下線で表示。
    /// スピナーはタイマーで、結果はストリーミングの到着で、それぞれ再描画される
    private func refreshTranslatingMarkedText(client: IMKTextInput) {
        let spinner = Self.spinnerFrames[translationSpinnerIndex % Self.spinnerFrames.count]
        let text = translationPartial.isEmpty
            ? "\(translatingJapanese) ⇢ \(translationLabel) \(spinner)"
            : "\(translatingJapanese) ⇢ \(translationPartial) \(spinner)"
        let attributed = NSAttributedString(
            string: text,
            attributes: [
                .underlineStyle: NSUnderlineStyle.single.rawValue,
                .underlineColor: NSColor.secondaryLabelColor,
            ]
        )
        client.setMarkedText(
            attributed,
            selectionRange: NSRange(location: text.utf16.count, length: 0),
            replacementRange: NSRange(location: NSNotFound, length: NSNotFound)
        )
    }

    private func startTranslationSpinner() {
        translationSpinnerTimer?.invalidate()
        let timer = Timer(timeInterval: 0.15, repeats: true) { [weak self] _ in
            guard let self, self.isTranslating else { return }
            self.translationSpinnerIndex += 1
            if let client = self.client() {
                self.refreshTranslatingMarkedText(client: client)
            }
        }
        RunLoop.main.add(timer, forMode: .common)
        translationSpinnerTimer = timer
    }

    private func stopTranslationSpinner() {
        translationSpinnerTimer?.invalidate()
        translationSpinnerTimer = nil
    }

    /// 翻訳待ちを取り消し、元の未確定表示に戻す
    private func cancelTranslation(client: IMKTextInput?) {
        translationGeneration += 1
        translationTask?.cancel()
        translationTask = nil
        isTranslating = false
        stopTranslationSpinner()
        guard let client else { return }
        if mode == .segmenting {
            refreshSegmentDisplay(client: client)
        } else {
            updateMarkedText(client: client, display: currentDisplay)
        }
    }

    /// 指定文字列を確定して状態をリセットする。
    /// `suggestsCompletion` が真なら、確定後の休止を待ってインライン補完を出す
    private func commitText(_ text: String, client: IMKTextInput?, suggestsCompletion: Bool = true) {
        cancelPrediction()
        dismissCompletion()
        // どの経路の確定でも進行中の英訳を無効化する（翻訳完了からの確定も含む）
        isTranslating = false
        translationGeneration += 1
        translationTask?.cancel()
        translationTask = nil
        stopTranslationSpinner()
        cancelConversion()
        composer = Self.makeComposer()  // 設定（句読点スタイル）の変更もここで反映される
        fixedChunks = []
        alphabetRun = nil
        segments = []
        segmentBaseline = nil
        // 候補ウィンドウを閉じる。文節変換中に文字を打って確定した場合など、
        // hidePanelを経由しない確定経路でパネルが残るのを防ぐ
        hidePanel()
        displayOverride = nil
        autoCommitPending = false
        mode = .composing
        guard let client else { return }
        client.insertText(text, replacementRange: NSRange(location: NSNotFound, length: NSNotFound))
        client.setMarkedText(
            NSAttributedString(string: ""),
            selectionRange: NSRange(location: 0, length: 0),
            replacementRange: NSRange(location: NSNotFound, length: NSNotFound)
        )
        recentCommitted = String((recentCommitted + text).suffix(LeftContext.maxLength))
        documentContext = nil
        if suggestsCompletion { scheduleCompletion(client: client, committed: text) }
    }

    /// 合成が始まる瞬間（未確定文字列がまだ無い）にアプリのカーソル手前のテキストを1回だけ読む。
    /// 合成中は読み直さない（未確定文字列が混ざる・同期IPCが増える）
    private func captureDocumentContextIfStarting(client: IMKTextInput) {
        guard !isComposing else { return }
        documentContext = DocumentContextSettings.read(from: client)
    }

    // MARK: - 予測変換（確定前）

    /// 予測変換: キー入力の休止（既定300ms、設定で変更可）後に、いま表示している未確定文字列の続き
    /// （次の文節）を予測してカーソル下の小窓に出す。表示が確定していないとき（ローマ字が未解決・ライブ変換の到着待ち・
    /// 英字入力中・F6等の上書き中）は何もしない。ライブ変換が届いた時点でもう一度呼ばれる。
    /// 文脈はモデルに「確定済みの文字列 + 表示中の未確定文字列」として渡す
    private func schedulePrediction() {
        predictionTask?.cancel()
        predictionTask = nil
        guard PredictionSettings.isPredictiveEnabled, liveConversionEnabled,
              mode == .composing, isComposing, alphabetRun == nil, displayOverride == nil,
              composer.pending.isEmpty,
              composer.isEmpty || lastConversion?.reading == composer.text
        else { return }
        let base = currentDisplay
        guard !base.isEmpty else { return }
        let context = conversionContext + base
        predictionGeneration += 1
        let generation = predictionGeneration
        let delay = remainingIdleDelay()
        predictionTask = Task { [weak self] in
            guard let self else { return }
            do {
                try await Task.sleep(for: delay)
                // 休止中に状態が変わっていたら推論しない（ライブ変換と推論を取り合わないように）
                let stillValid = await MainActor.run {
                    generation == self.predictionGeneration && self.mode == .composing
                        && self.currentDisplay == base
                }
                guard stillValid else { return }
                let text = try await Self.predictionEngine.predict(
                    context: context, maxLength: PredictionSettings.maxLength)
                guard !Task.isCancelled, !text.isEmpty else { return }
                await MainActor.run {
                    guard generation == self.predictionGeneration, self.mode == .composing,
                          self.currentDisplay == base, let client = self.client() else { return }
                    // 小窓を出せた（カーソル位置が取れた）ときだけTabで取り入れられる状態にする
                    if self.showPredictionPanel(text, client: client, markedTextLength: base.utf16.count) {
                        self.prediction = (base, text)
                    }
                }
            } catch is CancellationError {
            } catch {
                NSLog("iroha: 予測エラー: \(error)")
            }
        }
    }

    /// 進行中・表示中の予測を捨て、小窓を閉じる
    private func cancelPrediction() {
        predictionTask?.cancel()
        predictionTask = nil
        predictionGeneration += 1
        if prediction != nil {
            prediction = nil
            PredictionPanel.shared.hide()
        }
    }

    /// Tab: 表示中の予測を未確定文字列に取り入れる（確定はしない）。
    /// 入力中のかなはその時点のライブ変換結果で固定し、予測は読みの無い固定部分として続ける。
    /// 取り入れた直後からまた休止を待って次を予測するので、Tabを続けて押すと文が伸びていく
    private func acceptPrediction(client: IMKTextInput) -> Bool {
        guard let prediction, prediction.base == currentDisplay else { return false }
        cancelPrediction()
        lockComposerWithLiveConversion()
        fixedChunks.append(FixedChunk(reading: prediction.text, text: prediction.text, kind: .prediction))
        updateMarkedText(client: client, display: currentDisplay)
        schedulePrediction()
        return true
    }

    /// Backspace: 直前に取り入れた予測を取り消す。予測を取り入れたときに固定したかなが
    /// その前にあれば入力中の状態に戻す（Tabを押す前の表示に戻る）
    private func undoLastPrediction() {
        guard fixedChunks.last?.kind == .prediction else { return }
        fixedChunks.removeLast()
        conversionTask?.cancel()
        conversionTask = nil
        guard let last = fixedChunks.last, last.kind == .liveConversion else { return }
        fixedChunks.removeLast()
        composer.prependText(last.reading)
        lastConversion = (last.reading, last.text)
    }

    /// 最後のキー入力から休止時間が経つまでの残り
    private func remainingIdleDelay() -> Duration {
        let elapsed = lastKeyEventTime.duration(to: .now)
        let delay = PredictionSettings.idleDelay
        return elapsed >= delay ? .zero : delay - elapsed
    }

    /// 予測文をカーソル行の直下の小窓に出す。カーソル位置を教えてくれないアプリでは出さず、falseを返す。
    /// `markedTextLength` は未確定文字列のUTF-16長（カーソルはその末尾。無ければ0）
    private func showPredictionPanel(_ text: String, client: IMKTextInput, markedTextLength: Int) -> Bool {
        guard let rect = caretRect(client: client, markedTextLength: markedTextLength) else { return false }
        PredictionPanel.shared.show(text, near: rect)
        return true
    }

    /// カーソル（未確定文字列の末尾）の位置を表す幅0の矩形（スクリーン座標、高さは行の高さ）。
    ///
    /// `attributes(forCharacterIndex:lineHeightRectangle:)` は「その位置にある文字」の矩形を返し、
    /// 末尾（文字数と同じインデックス）を渡すと先頭に丸めるアプリが多い。そこで末尾の文字（length-1）の
    /// 矩形を取り、その右端をカーソルの位置とする。未確定文字列が無い（確定後）ときと、
    /// 末尾の文字の矩形が取れないときは先頭（=挿入位置）の矩形の左端。ゼロ矩形しか返さないアプリではnil
    private func caretRect(client: IMKTextInput, markedTextLength: Int) -> NSRect? {
        if markedTextLength > 0 {
            var rect = NSRect.zero
            _ = client.attributes(forCharacterIndex: markedTextLength - 1, lineHeightRectangle: &rect)
            if rect.height > 0 || rect.origin != .zero {
                return NSRect(x: rect.maxX, y: rect.minY, width: 0, height: rect.height)
            }
        }
        var rect = NSRect.zero
        _ = client.attributes(forCharacterIndex: 0, lineHeightRectangle: &rect)
        guard rect.height > 0 || rect.origin != .zero else { return nil }
        return NSRect(x: rect.minX, y: rect.minY, width: 0, height: rect.height)
    }

    // MARK: - インライン補完（確定後）

    /// インライン補完: 確定後、キー入力の休止（既定300ms、設定で変更可）を待って確定した文章の続き
    /// （次の文節）を予測し、カーソル下の小窓に出す。アプリのテキストにはTabで確定するまで触らない。
    /// 文脈はアプリのカーソル手前のテキスト（読めれば。確定した文字列で終わっていることを確認する）、
    /// なければ直前の確定文字列（最大40文字）。フォーカスが移ると後者は空になるので出ない
    private func scheduleCompletion(client: IMKTextInput, committed: String) {
        completionTask?.cancel()
        completionTask = nil
        guard PredictionSettings.isCompletionEnabled, japaneseMode, mode == .composing,
              !isComposing, !isTranslating, pendingCompletion == nil, !recentCommitted.isEmpty
        else { return }
        let committedSnapshot = recentCommitted
        var context = committedSnapshot
        if let document = DocumentContextSettings.read(from: client),
           document.hasSuffix(String(committed.suffix(LeftContext.maxLength / 2))) {
            context = document
        }
        completionGeneration += 1
        let generation = completionGeneration
        let delay = remainingIdleDelay()
        completionTask = Task { [weak self] in
            guard let self else { return }
            do {
                try await Task.sleep(for: delay)
                // 休止中に入力が始まっていたら推論しない
                let stillValid = await MainActor.run {
                    generation == self.completionGeneration && self.mode == .composing
                        && !self.isComposing && !self.isTranslating && self.recentCommitted == committedSnapshot
                }
                guard stillValid else { return }
                let text = try await Self.completionEngine.predict(
                    context: context, maxLength: PredictionSettings.maxLength)
                guard !Task.isCancelled, !text.isEmpty else { return }
                await MainActor.run {
                    guard generation == self.completionGeneration, self.mode == .composing,
                          !self.isComposing, !self.isTranslating, self.pendingCompletion == nil,
                          self.recentCommitted == committedSnapshot, let client = self.client() else { return }
                    // 未確定文字列が無いので先頭（=挿入位置）の矩形を使う
                    if self.showPredictionPanel(text, client: client, markedTextLength: 0) {
                        self.pendingCompletion = text
                    }
                }
            } catch is CancellationError {
            } catch {
                NSLog("iroha: 補完エラー: \(error)")
            }
        }
    }

    /// 小窓に出している補完を取り入れずに閉じる（進行中の予測も止める）
    private func dismissCompletion() {
        completionTask?.cancel()
        completionTask = nil
        completionGeneration += 1
        guard pendingCompletion != nil else { return }
        pendingCompletion = nil
        PredictionPanel.shared.hide()
    }

    /// Tab: 小窓に出している補完をアプリに挿入して確定する。確定後はまた休止を待って次の続きを出す
    private func acceptCompletion(client: IMKTextInput) {
        guard let text = pendingCompletion else { return }
        commitText(text, client: client)
    }

    /// 候補ウィンドウ操作用の合成キーイベント。
    /// interpretKeyEventsはcharactersの関数キーコード（U+F700系）を見て
    /// moveUp:/moveDown:に振り分けるため、実際の矢印キーと同じ文字を入れる必要がある
    private static func syntheticArrowEvent(keyCode: Int) -> NSEvent {
        let functionKey: String
        switch keyCode {
        case kVK_UpArrow: functionKey = "\u{F700}"    // NSUpArrowFunctionKey
        case kVK_DownArrow: functionKey = "\u{F701}"  // NSDownArrowFunctionKey
        default: functionKey = ""
        }
        return NSEvent.keyEvent(
            with: .keyDown,
            location: .zero,
            modifierFlags: .function,
            timestamp: ProcessInfo.processInfo.systemUptime,
            windowNumber: 0,
            context: nil,
            characters: functionKey,
            charactersIgnoringModifiers: functionKey,
            isARepeat: false,
            keyCode: UInt16(keyCode)
        )!
    }
}

// キーコード定数（Carbon/HIToolboxの値）
private let kVK_Return = 0x24
private let kVK_Tab = 0x30
private let kVK_Space = 0x31
private let kVK_Delete = 0x33
private let kVK_Escape = 0x35
private let kVK_ANSI_KeypadEnter = 0x4C
private let kVK_F6 = 0x61
private let kVK_F7 = 0x62
private let kVK_F8 = 0x64
private let kVK_F9 = 0x65
private let kVK_F10 = 0x6D
private let kVK_JIS_Eisu = 0x66
private let kVK_JIS_Kana = 0x68
private let kVK_LeftArrow = 0x7B
private let kVK_RightArrow = 0x7C
private let kVK_DownArrow = 0x7D
private let kVK_UpArrow = 0x7E
