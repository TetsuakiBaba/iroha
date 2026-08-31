import Cocoa
import InputMethodKit
import IrohaCore

/// キーイベントを処理するIMEコントローラ。
///
/// - ライブ変換: 入力のたびに読みをLLM（zenz-v3）へ送り、変換結果を未確定文字列として表示
/// - スペースキー: 文節変換モードへ（←→で文節移動、Shift+←→で伸縮、Spaceで候補ウィンドウ）
/// - Enterで確定、Escでかな表示に戻す/取消、BSで編集
/// - F6-F10 / Ctrl+U,I,O,P,T: ひらがな/カタカナ/半角カナ/全角英数/半角英数
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
    /// 学習 → ユーザ辞書 → LLM の順にデコレータで包む
    /// （どちらも空なら素通しなのでふるまいは変わらない）
    private static let engine: any ConversionEngine = LearningEngine(
        base: UserDictionaryEngine(base: ZenzEngine(modelPath: engineModelPath)),
        dictionary: { LearningSettings.dictionary })

    private enum Mode {
        case composing          // 入力・ライブ変換中
        case segmenting         // 文節変換中（スペースキー押下後）
    }

    /// 文節変換中の1文節
    private struct BunsetsuSegment {
        var reading: String         // ひらがなの読み
        var result: String          // 現在選ばれている変換結果
        var candidates: [String]?   // 取得済みの候補（キャッシュ）
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
        UserDefaults.standard.object(forKey: Self.commitOnPunctuationKey) as? Bool ?? true
    }
    /// 英訳確定（修飾キー+Enter）に使う修飾キー。nilなら機能オフ
    private var translateCommitModifier: NSEvent.ModifierFlags? {
        AICommitSettings.modifier(
            forKey: AICommitSettings.translateModifierKey, fallback: "control")
    }
    /// AI変換確定（修飾キー+Enter、プロンプトは設定）に使う修飾キー。nilなら機能オフ
    private var aiCommitModifier: NSEvent.ModifierFlags? {
        AICommitSettings.modifier(forKey: AICommitSettings.customModifierKey, fallback: "off")
    }

    // MARK: 状態

    /// F6/F7等による表示の強制上書き（ひらがな・カタカナ・英数）。次の入力でクリア
    private var displayOverride: String?
    /// 句読点入力後、変換結果の到着を待って自動確定するフラグ
    private var autoCommitPending = false
    /// 最後に完了したライブ変換の（読み, 変換結果）
    private var lastConversion: (reading: String, result: String)?
    private var conversionTask: Task<Void, Never>?
    /// 文脈条件付けに使う直前の確定文字列（最大40文字）
    private var recentCommitted = ""

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
    private var translationPartial = ""
    private var translationSpinnerIndex = 0
    private var translationSpinnerTimer: Timer?
    private static let spinnerFrames = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]

    private var isComposing: Bool { !composer.isEmpty }

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
        // モデルを事前ロード（未ロード時のみ実処理が走る）
        Task { try? await Self.engine.prewarm() }
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
                commitCurrent(client: sender as? IMKTextInput)
                japaneseMode = newJapaneseMode
            }
        }
        super.setValue(value, forTag: tag, client: sender)
    }

    override func handle(_ event: NSEvent!, client sender: Any!) -> Bool {
        guard let event, event.type == .keyDown,
              let client = sender as? IMKTextInput else { return false }

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
                commitCurrent(client: client)
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

        // 修飾キー+Enter: 未確定文字列をAIで処理して確定（修飾キーは設定で変更可能）
        if Int(event.keyCode) == kVK_Return || Int(event.keyCode) == kVK_ANSI_KeypadEnter,
           isComposing || mode == .segmenting {
            let pressed = event.modifierFlags.intersection([.command, .control, .option, .shift])
            if let modifier = translateCommitModifier, pressed == modifier {
                return handleAICommit(.translate, client: client)
            }
            if let modifier = aiCommitModifier, pressed == modifier {
                return handleAICommit(
                    .custom(prompt: AICommitSettings.customPrompt), client: client)
            }
        }

        // Command/Control付きのキーはIMEでは扱わない（未確定文字列は確定して逃がす）
        if event.modifierFlags.contains(.command) || event.modifierFlags.contains(.control) {
            if isComposing { commitCurrent(client: client) }
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
        commitCurrent(client: sender as? IMKTextInput)
        super.deactivateServer(sender)
    }

    override func commitComposition(_ sender: Any!) {
        commitCurrent(client: sender as? IMKTextInput)
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

        menu.addItem(NSMenuItem.separator())

        let liveItem = NSMenuItem(
            title: "ライブ変換",
            action: #selector(toggleLiveConversion(_:)),
            keyEquivalent: ""
        )
        liveItem.target = self
        liveItem.state = liveConversionEnabled ? .on : .off
        menu.addItem(liveItem)

        let punctItem = NSMenuItem(
            title: "句読点で自動確定",
            action: #selector(toggleCommitOnPunctuation(_:)),
            keyEquivalent: ""
        )
        punctItem.target = self
        punctItem.state = commitOnPunctuationEnabled ? .on : .off
        menu.addItem(punctItem)

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
            updateMarkedText(client: client, display: composer.display)
        }
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

    @objc private func toggleCommitOnPunctuation(_ sender: Any?) {
        UserDefaults.standard.set(!commitOnPunctuationEnabled, forKey: Self.commitOnPunctuationKey)
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
        let dir = NSHomeDirectory() + "/Library/Application Support/iroha/models"
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        NSWorkspace.shared.open(URL(fileURLWithPath: dir))
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
            displayOverride = nil
            autoCommitPending = false
            composer.deleteBackward()
            composerDidChange(client: client)
            return true
        case kVK_Escape:
            guard isComposing else { return false }
            autoCommitPending = false
            if displayOverride != nil {
                // F6/F7等の上書き表示をやめてかなに戻す
                displayOverride = nil
                updateMarkedText(client: client, display: composer.display)
            } else if lastConversion != nil {
                // 1回目のEsc: 変換をやめてかな表示に戻す
                cancelConversion()
                updateMarkedText(client: client, display: composer.display)
            } else {
                // 2回目のEsc: 入力自体を取り消す
                composer = Self.makeComposer()
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
            if isComposing { commitCurrent(client: client) }
            return false
        }
        displayOverride = nil
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
            updateMarkedText(client: client, display: converted)
        }
        return true  // 変換できない場合もキーは消費する（Fキーがアプリに漏れないように）
    }

    // MARK: - 文節変換モード

    /// スペース押下: 全体を変換し、文節に分割して文節変換モードに入る
    private func enterSegmentMode(client: IMKTextInput) {
        conversionTask?.cancel()
        autoCommitPending = false
        displayOverride = nil
        composer.flush()
        let reading = composer.text
        guard !reading.isEmpty else { return }

        mode = .segmenting
        segmentGeneration += 1
        let generation = segmentGeneration

        // 暫定表示: 全体を1文節として今の表示内容をそのまま使う
        let interim = (lastConversion?.reading == reading) ? lastConversion!.result : reading
        segments = [BunsetsuSegment(reading: reading, result: interim, candidates: nil)]
        segmentBaseline = (lastConversion?.reading == reading) ? lastConversion?.result : nil
        currentSegmentIndex = 0
        refreshSegmentDisplay(client: client)

        let context = recentCommitted
        let cachedConversion = (lastConversion?.reading == reading) ? lastConversion?.result : nil
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
                    self.segmentBaseline = full
                    self.segments = aligned.map {
                        BunsetsuSegment(reading: $0.reading, result: $0.conversion, candidates: nil)
                    }
                    self.currentSegmentIndex = 0
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
                composer.input(first)
                composerDidChange(client: client)
                return true
            }
            commitSegments(client: client)
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
        let context = recentCommitted + segments[..<index].map(\.result).joined()
        let generation = segmentGeneration
        let count = candidateCount
        Task { [weak self] in
            guard let self else { return }
            do {
                var candidates = try await Self.engine.convert(
                    reading: reading, context: context, candidateCount: count)
                // 定番のフォールバック候補（ひらがな・カタカナ）を末尾に追加
                for extra in [reading, hiraganaToKatakana(reading)] where !candidates.contains(extra) {
                    candidates.append(extra)
                }
                let finalCandidates = candidates
                await MainActor.run {
                    guard self.mode == .segmenting, generation == self.segmentGeneration,
                          self.currentSegmentIndex == index else { return }
                    self.segments[index].candidates = finalCandidates
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
        let context = recentCommitted + segments[..<index].map(\.result).joined()
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
        panelCandidates = []
        cancelConversion()
        updateMarkedText(client: client, display: composer.display)
    }

    /// 全文節の変換結果を結合して確定する
    private func commitSegments(client: IMKTextInput?) {
        let text = segments.map(\.result).joined()
        learnIfCorrected(committed: text)
        commitText(text.isEmpty ? composer.display : text, client: client)
    }

    /// 文節変換の結果がエンジンの出力と違っていたら、ユーザによる修正として学習する。
    /// （修正しなかった文節も、位置ごとの文脈つきで一緒に覚える。
    /// そうしないと「きしゃのきしゃ」の後半が次回また第一候補に戻ってしまう）
    private func learnIfCorrected(committed: String) {
        guard LearningSettings.isEnabled, !segments.isEmpty, !committed.isEmpty,
              let baseline = segmentBaseline, committed != baseline else { return }
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
        // 変換済みの読みと同じなら再変換不要
        if lastConversion?.reading == reading { return }

        let context = recentCommitted
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
        if let displayOverride { return displayOverride }
        if let lastConversion {
            if lastConversion.reading == composer.text {
                return lastConversion.result + composer.pending
            }
            if composer.text.hasPrefix(lastConversion.reading) {
                let addedKana = composer.text.dropFirst(lastConversion.reading.count)
                return lastConversion.result + addedKana + composer.pending
            }
        }
        return composer.display
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

    /// 現在の表示内容（ライブ変換結果 or かな or 文節列）をそのまま確定する
    private func commitCurrent(client: IMKTextInput?) {
        if mode == .segmenting { hidePanel() }
        guard let text = resolveCommitText() else { return }
        commitText(text, client: client)
    }

    /// 通常確定で挿入されるはずの文字列を解決する（composer.flushの副作用あり。
    /// 呼び出し後は必ずcommitTextするか、状態を破棄/維持したまま確定を待つこと）
    private func resolveCommitText() -> String? {
        if mode == .segmenting {
            let text = segments.map(\.result).joined()
            return text.isEmpty ? composer.display : text
        }
        guard isComposing else { return nil }
        if let displayOverride {
            // F6/F7等で上書き表示中はその内容を確定する
            return displayOverride
        }
        // 未解決ローマ字を確定（"n"→"ん"）。flushで増えたかなは変換結果の後ろに付ける
        let readingBeforeFlush = composer.text
        composer.flush()
        let flushedSuffix = String(composer.text.dropFirst(readingBeforeFlush.count))
        if let lastConversion, lastConversion.reading == readingBeforeFlush {
            return lastConversion.result + flushedSuffix
        }
        if let lastConversion, readingBeforeFlush.hasPrefix(lastConversion.reading) {
            // 変換が追いつく前の確定: 表示と同じく「変換済み接頭辞 + 追加のかな」を確定する
            let addedKana = readingBeforeFlush.dropFirst(lastConversion.reading.count)
            return lastConversion.result + addedKana + flushedSuffix
        }
        return composer.display
    }

    // MARK: - AIで処理して確定（修飾キー+Enter）

    /// 現在の未確定文字列をAI（英訳 or ユーザ設定のプロンプト）で処理して確定する。
    /// 合成状態はリセットせず生かしたまま結果を待つ（Escで通常の未確定状態に戻れる）
    private func handleAICommit(_ action: AICommitAction, client: IMKTextInput) -> Bool {
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
        translationPartial = ""
        translationSpinnerIndex = 0
        refreshTranslatingMarkedText(client: client)
        startTranslationSpinner()

        translationTask = Task { [weak self] in
            // ストリーミング: 届いた結果を随時マークテキストに反映する
            let request = action.request(for: japanese)
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
            ? "\(translatingJapanese) ⇢ \(spinner)"
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

    /// 指定文字列を確定して状態をリセットする
    private func commitText(_ text: String, client: IMKTextInput?) {
        // どの経路の確定でも進行中の英訳を無効化する（翻訳完了からの確定も含む）
        isTranslating = false
        translationGeneration += 1
        translationTask?.cancel()
        translationTask = nil
        stopTranslationSpinner()
        cancelConversion()
        composer = Self.makeComposer()  // 設定（句読点スタイル）の変更もここで反映される
        segments = []
        segmentBaseline = nil
        panelCandidates = []
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
        recentCommitted = String((recentCommitted + text).suffix(40))
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
