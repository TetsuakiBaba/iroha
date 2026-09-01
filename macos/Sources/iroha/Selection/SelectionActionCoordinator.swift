import AppKit
import Foundation

/// 選択テキストのAI処理の中核。
/// グローバルショートカットとマウス選択トリガーを束ね、
/// 選択取得 → TranslationService実行 → パネル表示 → 置換/挿入 を調停する。
/// IMEの入力処理（IrohaInputController）とは独立に、IMEプロセス内で常駐する
@MainActor
final class SelectionActionCoordinator {
    static let shared = SelectionActionCoordinator()

    /// IMEが未確定文字列を持っている間はグローバルショートカットを無視する
    /// （IrohaInputController.handleが毎キーで更新する。メインスレッドのみで触る）
    nonisolated(unsafe) static var isIMEComposing = false

    private let hotKeyCenter = HotKeyCenter()
    private let selectionService = SelectionService()
    private let mouseMonitor = MouseSelectionMonitor()
    private lazy var bubbleController = SelectionBubbleController()
    private lazy var menuController = SelectionMenuController()
    private lazy var panelController: SelectionPanelController = makePanelController()

    /// 置換先（選択を取得したときのアプリとフォーカス要素）
    private var currentContext: SelectionContext?
    private var running = false
    /// 閉じた後に遅れて届いたストリーミング結果を無視する世代カウンタ
    private var generation = 0
    private var captureTask: Task<Void, Never>?
    private var reloadTask: Task<Void, Never>?
    /// 同じ選択でバブルを連続表示しないための指紋
    private var lastFingerprint: String?
    private var lastFingerprintDate = Date.distantPast
    private var hasPromptedPermission = false
    private var started = false

    private var model: SelectionPanelModel { panelController.model }

    // MARK: - 起動と設定反映

    func start() {
        guard !started else { return }
        started = true
        reload()
        // 設定ウィンドウでの変更を反映する（キー単位の通知は無いのでまとめて再登録。
        // 頻発するためデバウンスする）
        NotificationCenter.default.addObserver(
            forName: UserDefaults.didChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in
            Task { @MainActor [weak self] in self?.scheduleReload() }
        }
    }

    private func scheduleReload() {
        reloadTask?.cancel()
        reloadTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 500_000_000)
            guard !Task.isCancelled else { return }
            self?.reload()
        }
    }

    /// 設定を読み直してホットキーとマウス監視を再登録する
    private func reload() {
        hotKeyCenter.unregisterAll()
        mouseMonitor.stop()
        bubbleController.dismiss()

        guard SelectionSettings.isEnabled else { return }

        // 同じショートカットが複数の枠に入っていたら先勝ちにする（二重発火を防ぐ）
        var registered = Set<String>()
        for preset in SelectionSettings.presets where preset.enabled && !preset.hotkey.isEmpty {
            guard registered.insert(preset.hotkey.lowercased()).inserted else { continue }
            let index = preset.index
            hotKeyCenter.register(shortcut: preset.hotkey) { [weak self] in
                self?.handlePresetHotkey(index)
            }
        }
        let onDemand = SelectionSettings.onDemandHotkey
        if !onDemand.isEmpty, registered.insert(onDemand.lowercased()).inserted {
            hotKeyCenter.register(shortcut: onDemand) { [weak self] in
                self?.handleOnDemandHotkey()
            }
        }

        if SelectionSettings.triggerMode != .off {
            startMouseMonitor()
        }
    }

    // MARK: - ショートカットのトリガー

    private var canTrigger: Bool {
        SelectionSettings.isEnabled && !running && !Self.isIMEComposing
            && !panelController.isVisible
    }

    private func handlePresetHotkey(_ index: Int) {
        guard canTrigger else { return }
        let preset = SelectionSettings.preset(index)
        guard preset.enabled else { return }

        Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let capture = try await self.selectionService.captureSelectedText()
                guard !self.isExcluded(capture.context.bundleIdentifier) else { return }
                guard let selectedText = capture.selectedText else {
                    self.showNotice("テキストを選択してください")
                    return
                }
                self.currentContext = capture.context
                self.runAI(
                    title: preset.displayName,
                    request: preset.request(for: selectedText),
                    source: selectedText,
                    isGeneration: false
                )
            } catch {
                self.presentError(error.localizedDescription)
            }
        }
    }

    private func handleOnDemandHotkey() {
        guard canTrigger else { return }

        Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let capture = try await self.selectionService.captureSelectedText()
                guard !self.isExcluded(capture.context.bundleIdentifier) else { return }
                self.currentContext = capture.context
                self.presentPromptInput(sourceText: capture.selectedText)
            } catch {
                self.presentError(error.localizedDescription)
            }
        }
    }

    // MARK: - マウス選択のトリガー

    private func startMouseMonitor() {
        mouseMonitor.start { [weak self] in
            self?.bubbleController.dismiss()
            self?.captureTask?.cancel()
            self?.captureTask = nil
        } onSelectionGesture: { [weak self] location, modifiers in
            self?.handleMouseSelectionGesture(at: location, modifiers: modifiers)
        }
    }

    private func handleMouseSelectionGesture(at location: NSPoint, modifiers: NSEvent.ModifierFlags) {
        guard SelectionSettings.isEnabled, !running, !panelController.isVisible else { return }
        let mode = SelectionSettings.triggerMode
        guard mode != .off else { return }
        if mode == .optionMenu, !modifiers.contains(.option) { return }

        guard selectionService.ensureAccessibilityPermission(prompt: false) else {
            if !hasPromptedPermission {
                hasPromptedPermission = true
                _ = selectionService.ensureAccessibilityPermission(prompt: true)
            }
            return
        }

        // 選択直後はAXの選択情報が安定していないことがあるので少し待つ
        captureTask?.cancel()
        captureTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 160_000_000)
            guard !Task.isCancelled, let self else { return }
            self.captureTask = nil
            self.presentSelectionAction(at: location)
        }
    }

    private func presentSelectionAction(at location: NSPoint) {
        guard !isExcluded(NSWorkspace.shared.frontmostApplication?.bundleIdentifier) else { return }
        guard
            let capture = selectionService.captureAccessibleSelectedText(),
            let selectedText = capture.selectedText,
            !isExcluded(capture.context.bundleIdentifier)
        else {
            return
        }

        let fingerprint = [
            capture.context.bundleIdentifier ?? "",
            String(capture.context.selectedTextRange?.location ?? -1),
            String(capture.context.selectedTextRange?.length ?? -1),
            selectedText,
        ].joined(separator: "|")
        let now = Date()
        if fingerprint == lastFingerprint, now.timeIntervalSince(lastFingerprintDate) < 0.75 {
            return
        }
        lastFingerprint = fingerprint
        lastFingerprintDate = now

        switch SelectionSettings.triggerMode {
        case .bubble:
            bubbleController.present(at: location) { [weak self] in
                self?.presentMenu(for: capture, at: location)
            }
        case .immediateMenu, .optionMenu:
            presentMenu(for: capture, at: location)
        case .off:
            break
        }
    }

    private func presentMenu(for capture: SelectionCaptureResult, at location: NSPoint) {
        bubbleController.dismiss()
        menuController.present(at: location, presets: SelectionSettings.presets) { [weak self] choice in
            self?.handleMenuChoice(choice, capture: capture)
        }
    }

    private func handleMenuChoice(_ choice: SelectionMenuChoice, capture: SelectionCaptureResult) {
        guard let selectedText = capture.selectedText else { return }
        currentContext = capture.context

        switch choice {
        case .onDemand:
            presentPromptInput(sourceText: selectedText)
        case .preset(let index):
            let preset = SelectionSettings.preset(index)
            guard preset.enabled else { return }
            runAI(
                title: preset.displayName,
                request: preset.request(for: selectedText),
                source: selectedText,
                isGeneration: false
            )
        }
    }

    private func isExcluded(_ bundleIdentifier: String?) -> Bool {
        guard let bundleIdentifier else { return false }
        if bundleIdentifier.caseInsensitiveCompare(Bundle.main.bundleIdentifier ?? "") == .orderedSame {
            return true
        }
        return SelectionSettings.excludedBundleIdentifiers.contains(bundleIdentifier.lowercased())
    }

    // MARK: - パネルとAI実行

    private func makePanelController() -> SelectionPanelController {
        let controller = SelectionPanelController()
        controller.onSubmitPrompt = { [weak self] in self?.submitOnDemandPrompt() }
        controller.onApply = { [weak self] in self?.applyResult() }
        controller.onCopyResult = { [weak self] in self?.copyResult() ?? false }
        controller.onClose = { [weak self] in self?.closePanel() }
        return controller
    }

    /// オンデマンド: プロンプト入力状態でパネルを出す（選択なしなら生成モード）
    private func presentPromptInput(sourceText: String?) {
        model.phase = .promptInput
        model.title = "AIに指示"
        model.sourceText = sourceText ?? ""
        model.outputText = ""
        model.promptText = ""
        model.notice = nil
        model.isGeneration = (sourceText == nil)
        panelController.present(near: NSEvent.mouseLocation)
    }

    private func submitOnDemandPrompt() {
        let prompt = model.promptText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !prompt.isEmpty else { return }

        let request: AIRequest
        if model.isGeneration {
            request = AIRequest(
                instructions: "ユーザの指示に従ってテキストを作成してください。\n"
                    + AICommitSettings.outputRule,
                userMessage: prompt)
        } else {
            request = AIRequest.build(prompt: prompt, text: model.sourceText)
        }
        runAI(
            title: "AIに指示",
            request: request,
            source: model.sourceText,
            isGeneration: model.isGeneration
        )
    }

    private func runAI(title: String, request: AIRequest, source: String, isGeneration: Bool) {
        guard TranslationService.isAvailable else {
            showNotice("AIサービスが利用できません（設定 > モデル を確認）")
            return
        }

        running = true
        generation += 1
        let gen = generation

        model.phase = .processing
        model.title = title
        model.sourceText = source
        model.outputText = ""
        model.notice = nil
        model.isGeneration = isGeneration

        if panelController.isVisible {
            panelController.refreshSize()
        } else {
            panelController.present(near: NSEvent.mouseLocation)
        }

        Task { [weak self] in
            let output = await TranslationService.run(request, onPartial: { partial in
                Task { @MainActor [weak self] in
                    guard let self, gen == self.generation else { return }
                    self.model.outputText = partial.trimmingCharacters(in: .whitespacesAndNewlines)
                    self.panelController.refreshSize()  // 内容に合わせて伸ばす
                }
            })
            guard let self else { return }
            await MainActor.run {
                guard gen == self.generation else { return }
                self.running = false
                if let output {
                    self.model.outputText = AIOutputCleaner.clean(output)
                } else {
                    // 失敗・タイムアウト: 部分結果があればそのまま残す（コピーはできる）
                    self.model.notice = "AI処理に失敗しました（サービス未起動またはタイムアウト）"
                }
                self.model.phase = .done
                self.panelController.refreshSize()
            }
        }
    }

    /// Return / 置換ボタン: 結果を選択範囲に適用する（生成モードならカーソル位置へ挿入）
    private func applyResult() {
        guard model.phase == .done, !model.outputText.isEmpty else { return }
        let text = model.outputText
        let isGeneration = model.isGeneration
        let context = currentContext
        closePanel()

        Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                if isGeneration {
                    try await self.selectionService.insertGeneratedText(text, context: context)
                } else {
                    try await self.selectionService.applyReplacement(text, context: context)
                }
            } catch {
                self.presentError(error.localizedDescription)
            }
        }
    }

    /// ⌘C / コピーボタン: 結果をペーストボードへ。falseならテキスト欄の通常コピーに任せる
    private func copyResult() -> Bool {
        guard model.phase == .done, !model.outputText.isEmpty else { return false }
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(model.outputText, forType: .string)
        model.notice = "コピーしました"
        return true
    }

    private func closePanel() {
        generation += 1  // 進行中のストリーミング結果を無効化
        running = false
        panelController.dismiss()
        currentContext = nil
    }

    /// 短い通知をパネルで出す（自動で消える）
    private func showNotice(_ text: String) {
        model.phase = .notice
        model.notice = text
        panelController.present(near: NSEvent.mouseLocation)
    }

    private func presentError(_ message: String) {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = "iroha"
        alert.informativeText = message
        alert.window.level = .modalPanel  // LSBackgroundOnlyでも前面に出す
        NSApp.activate(ignoringOtherApps: true)
        alert.runModal()
    }
}
