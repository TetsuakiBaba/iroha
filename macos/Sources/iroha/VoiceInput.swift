import ApplicationServices
import Carbon.HIToolbox
import Cocoa
import IrohaCore
import IrohaVoice

/// 「こえ」モードの設定（設定 > 入力 > こえ。開発者モードの項目・既定OFF・他のMacと同期しない
/// （マイクの許可もキーボードの形も機械ごとのため）。試作。2026-11-10）
enum VoiceInputSettings {
    static let enabledKey = "voiceInputEnabled"
    static let holdKeyKey = "voiceInputHoldKey"
    static let recognizerKey = "voiceInputRecognizer"
    static let otherInputSourcesKey = "voiceInputOtherInputSources"

    /// 押している間だけ録音するキー。修飾キー単独にする（普通のキーは離したことが入力メソッドに届かない）
    enum HoldKey: String, CaseIterable, Identifiable {
        case rightCommand
        case rightOption
        case function

        var id: String { rawValue }

        var keyCode: Int {
            switch self {
            case .rightCommand: return kVK_RightCommand
            case .rightOption: return kVK_RightOption
            case .function: return kVK_Function
            }
        }

        var flag: NSEvent.ModifierFlags {
            switch self {
            case .rightCommand: return .command
            case .rightOption: return .option
            case .function: return .function
            }
        }

        /// `CGEventSource.flagsState` の値のうち、このキーが押されていることを表すビット。
        /// 左右を区別するビット（IOKit の NX_DEVICERCMDKEYMASK = 0x10、NX_DEVICERALTKEYMASK = 0x40）で見る。
        /// `CGEventSource.keyState` は右側の修飾キーのキー番号を渡しても偽を返し、左側の番号に左右どちらの
        /// 状態も出るので使えない（2026-11-10 実測。右 Option を押すと keyState(58) が真、keyState(61) は偽）
        var deviceMask: UInt64 {
            switch self {
            case .rightCommand: return 0x10
            case .rightOption: return 0x40
            case .function: return CGEventFlags.maskSecondaryFn.rawValue
            }
        }

        /// いま押されているか（権限は要らない）
        var isDown: Bool {
            CGEventSource.flagsState(.combinedSessionState).rawValue & deviceMask != 0
        }

        /// このキー以外の修飾キー（左右の ⌘・Option・Shift・Control）が押されているか
        var otherModifiersDown: Bool {
            // 左 Control 0x01・左 Shift 0x02・右 Shift 0x04・左 ⌘ 0x08・右 ⌘ 0x10・左 Option 0x20・右 Option 0x40・右 Control 0x2000
            let all: UInt64 = 0x207F
            return CGEventSource.flagsState(.combinedSessionState).rawValue & all & ~deviceMask != 0
        }

        var label: String {
            switch self {
            case .rightCommand: return "右 ⌘"
            case .rightOption: return "右 Option"
            case .function: return "fn（地球儀）"
            }
        }
    }

    static var isEnabled: Bool {
        VoiceRecognition.isSupported && UserDefaults.standard.bool(forKey: enabledKey)
    }

    static var holdKey: HoldKey {
        UserDefaults.standard.string(forKey: holdKeyKey).flatMap(HoldKey.init(rawValue:)) ?? .rightCommand
    }

    /// iroha 以外の入力ソース（ABC・別の IME の英数など）を選んでいるときも、決めたキーで録音を始めるか
    /// （既定OFF・同期しない。2026-11-25）。録音を始めるときに入力ソースを iroha に切り替え、
    /// 切り替わって入力を受け持ったコントローラに認識結果を入れる。キーは修飾キーの変化を全体で受け取って見る
    /// （`NSEvent.addGlobalMonitorForEvents`。アクセシビリティの許可が要る）
    static var usesOtherInputSources: Bool {
        isEnabled && UserDefaults.standard.bool(forKey: otherInputSourcesKey)
    }

    static var recognizer: VoiceRecognizerKind {
        UserDefaults.standard.string(forKey: recognizerKey).flatMap(VoiceRecognizerKind.init(rawValue:)) ?? .speech
    }
}

/// 「こえ」モードの認識結果を記録するか（`VoiceLog`。設定 > 入力 > 「こえ」モード、開発者モードの項目・既定OFF・
/// 他のMacと同期しない（`ConversionLogSettings` と同じく、データフォルダを共有する別のMacで記録が始まらないように）。
/// フィラーの除去を評価する材料を集めるため。2026-11-19）
enum VoiceLogSettings {
    static let enabledKey = "voiceLogEnabled"

    static var isEnabled: Bool {
        UserDefaults.standard.bool(forKey: enabledKey)
    }

    /// いま記録するか。設定がONでも、プライベートモードの間は記録しない
    static var isActive: Bool {
        isEnabled && !PrivateModeSettings.isEnabled
    }
}

/// 「こえ」モード: 決めた修飾キーを押している間だけ録音し、離したら認識結果を入力中のコントローラに渡す（試作。2026-11-10）。
///
/// 認識結果は確定せず、読みに戻して未確定文字列に入れる（`IrohaInputController.insertVoiceTranscript`）。
/// キーボードで打った読みと同じ扱いになるので、ライブ変換・Space の候補・文節の区切り直しで直せる。
///
/// キーの扱い:
/// - 押してから少し（`startDelay`）待って録音を始める。その前に離した・他のキーを押した（⌘C など）なら何もしない
/// - 録音中に他のキー・クリック・他の修飾キーが入ったら、ショートカットに使ったとみなして捨てる
/// - 押していた時間が短すぎる（`minimumHold`）なら捨てる
/// - 押した・離したは入力メソッドに届く修飾キーの変化（`flagsChanged`）で受け、取りこぼしに備えて
///   修飾キーの状態（`CGEventSource.flagsState` の左右を区別するビット。権限は要らない）も 30ms ごとに読む
///
/// マイクの許可のダイアログはここでは出さない（未許可なら知らせを出すだけ。許可は設定画面から求める。
/// ダイアログに答えるまで入力メソッドとしての応答が止まるのを避ける）。メインスレッドから使う
final class VoiceInput {
    static let shared = VoiceInput()

    private enum Phase {
        case idle
        /// キーを押した。録音を始めるまでの待ち
        case waiting
        case recording
        /// キーを離した。認識の結果を待っている
        case finishing
    }

    private var phase = Phase.idle
    /// 押すたびに進める。古い回の非同期の続きを無効にする
    private var generation = 0
    private var holdKey = VoiceInputSettings.HoldKey.rightCommand
    private var pressedAt = 0.0
    private var pollTimer: Timer?
    /// いま入力を受け持っているコントローラ（キーの状態を読んで押したことを見つけたときの入力先）
    private weak var activeController: IrohaInputController?
    /// 前に読んだときに決めたキーが押されていたか（押した瞬間だけを拾う）
    private var wasDown = false
    private weak var controller: IrohaInputController?
    /// 録音中の 1 回ぶん（`VoiceSession`。macOS 26 以降でだけ作る）
    private var session: AnyObject?
    private var startTask: Task<Void, Error>?
    private var lastPartial = ""
    /// 録音を始めるときに入力ソースを iroha に切り替える回か（iroha 以外の入力ソースのときに押した）
    private var switchesInputSource = false
    /// 入力ソースを iroha に切り替えた時刻
    private var switchedAt: Double?
    /// 修飾キーの変化を全体で受け取る見張り（`VoiceInputSettings.usesOtherInputSources`）
    private var globalMonitor: Any?
    /// 見張りを付けたときにアクセシビリティの許可があったか（許可の前に付けた見張りにはキーが届かないので付け直す）
    private var globalMonitorTrusted = false
    private var started = false
    /// 途中結果を入力先の未確定文字列として見せているか（小窓には出さない）
    private var partialShownInPlace = false

    /// 押してから録音を始めるまでの待ち（ショートカットに使うたびにマイクが動かないように）
    private static let startDelay = 0.12
    /// iroha に切り替えてから、受け持ちが外れても録音を続ける時間
    private static let switchSettleTime = 1.0
    /// 押していた時間がこれより短ければ捨てる
    private static let minimumHold = 0.35
    /// 押し続けていてもここで締める
    private static let maximumHold = 60.0
    private static let pollInterval = 0.03
    /// 小窓に出す途中結果の長さ（末尾から）
    private static let partialLength = 28

    private init() {}

    /// 録音中・認識待ちか（このあいだ修飾キーの変化を受け取り続ける）
    var isBusy: Bool { phase != .idle }

    /// 起動時に 1 回呼ぶ（main.swift）。以後は設定が変わるたび・前面のアプリが替わるたびに、全体の見張りを合わせる
    func start() {
        guard !started else { return }
        started = true
        syncGlobalMonitor()
        NotificationCenter.default.addObserver(
            forName: UserDefaults.didChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in self?.syncGlobalMonitor() }
        // アクセシビリティの許可は設定を変えずに与えられるので、アプリが替わるたびにも確かめる
        NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didActivateApplicationNotification, object: nil, queue: .main
        ) { [weak self] _ in self?.syncGlobalMonitor() }
    }

    private func syncGlobalMonitor() {
        let wanted = VoiceInputSettings.usesOtherInputSources
        let trusted = AXIsProcessTrusted()
        if globalMonitor != nil, !wanted || trusted != globalMonitorTrusted {
            if let globalMonitor { NSEvent.removeMonitor(globalMonitor) }
            globalMonitor = nil
        }
        guard wanted, globalMonitor == nil else { return }
        globalMonitorTrusted = trusted
        globalMonitor = NSEvent.addGlobalMonitorForEvents(matching: .flagsChanged) { event in
            VoiceInput.shared.handleGlobalFlagsChanged(event)
        }
    }

    /// ほかのアプリに届いた修飾キーの変化（全体の見張り）。iroha が入力を受け持っていないときだけ扱う
    /// （受け持っているときは入力メソッドに同じ変化が届く）
    private func handleGlobalFlagsChanged(_ event: NSEvent) {
        if phase == .idle {
            guard VoiceInputSettings.usesOtherInputSources, activeController == nil,
                  !IrohaInputSource.isCurrent else { return }
        } else if controller != nil || !switchesInputSource {
            return
        }
        handleFlagsChanged(event, controller: nil)
    }

    /// 入力メソッドに届いた修飾キーの変化（`controller` が nil なら全体の見張りから。入力ソースを iroha に切り替えて入れる）
    func handleFlagsChanged(_ event: NSEvent, controller: IrohaInputController?) {
        guard VoiceInputSettings.isEnabled || phase != .idle else { return }
        let key = phase == .idle ? VoiceInputSettings.holdKey : holdKey
        guard Int(event.keyCode) == key.keyCode else {
            // 他の修飾キーが動いた: ショートカットに使っている
            if phase == .waiting || phase == .recording { cancel() }
            return
        }
        // 入力メソッドに届くイベントの修飾キーには左右の区別が入っていないので、押されているかは OS の状態で見る
        let down = key.isDown
        if !down {
            release()
        } else if phase == .idle, !key.otherModifiersDown {
            press(key, controller: controller)
        }
    }

    /// 入力メソッドに届いたキー（押している間に来たら、ショートカットに使ったとみなして捨てる）
    func noteKeyDown() {
        if phase == .waiting || phase == .recording { cancel() }
    }

    /// コントローラが入力を受け持った。設定が ON なら、決めたキーの状態を読み始める
    /// （修飾キーの変化が入力メソッドに届かない場合に備える。届けばそちらが先に拾う）
    func controllerActivated(_ controller: IrohaInputController) {
        activeController = controller
        // iroha に切り替えて録音している回: 切り替わって受け持ったコントローラを入れ先にする
        if phase != .idle, switchesInputSource, self.controller == nil {
            self.controller = controller
            if !lastPartial.isEmpty, controller.showVoicePartial(lastPartial) { partialShownInPlace = true }
        }
        if VoiceInputSettings.isEnabled { startPolling() }
    }

    /// 入力先が変わった（そのコントローラ宛ての録音は捨てる）
    func controllerDeactivated(_ controller: IrohaInputController) {
        if activeController === controller { activeController = nil }
        if self.controller === controller, phase != .idle, switchesInputSource,
           let switchedAt, Self.now() - switchedAt < Self.switchSettleTime {
            // iroha に切り替えた直後は、受け持ちが「始める → 外れる → 始める」と動く（2026-11-25 実測。間は 1ms ほど）。
            // 外れたら入れ先を外して、次に受け持ったコントローラを入れ先にする
            controller.clearVoicePartial()
            partialShownInPlace = false
            self.controller = nil
        } else if self.controller === controller, phase != .idle {
            cancel()
        } else if phase == .idle, activeController == nil {
            stopPolling()
        }
    }

    private func press(_ key: VoiceInputSettings.HoldKey, controller: IrohaInputController?) {
        generation += 1
        let generation = self.generation
        phase = .waiting
        holdKey = key
        pressedAt = Self.now()
        wasDown = true
        self.controller = controller
        switchesInputSource = controller == nil
        lastPartial = ""
        partialShownInPlace = false
        startPolling()
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.startDelay) { [weak self] in
            guard let self, generation == self.generation, self.phase == .waiting else { return }
            self.startRecording()
        }
    }

    private func startRecording() {
        guard VoiceMicrophone.status == .authorized else {
            reset()
            TapInputFeedback.shared.show("こえ: マイクが未許可です（設定 > 入力 > こえ）", duration: 3)
            return
        }
        guard #available(macOS 26.0, *) else {
            reset()
            return
        }
        // iroha 以外の入力ソースで押した: ここで iroha に切り替える（ショートカットに使ったときに切り替えないよう、
        // 押してすぐではなく録音を始めるときに行う）。受け持ったコントローラは `controllerActivated` で入れ先になる
        if switchesInputSource, controller == nil, !IrohaInputSource.selectJapanese() {
            reset()
            TapInputFeedback.shared.show("こえ: iroha に切り替えられませんでした", duration: 3)
            return
        }
        if switchesInputSource, controller == nil { switchedAt = Self.now() }
        let generation = self.generation
        let session = VoiceSession(kind: VoiceInputSettings.recognizer) { text in
            DispatchQueue.main.async { VoiceInput.shared.receivePartial(text, generation: generation) }
        }
        self.session = session
        phase = .recording
        TapInputFeedback.shared.show("● 聞いています", duration: Self.maximumHold + 5)
        // マイクの立ち上げは数十ミリ秒かかるのでメインスレッドの外で行う。終了はこれを待ってから進める
        startTask = Task.detached(priority: .userInitiated) { try session.start() }
    }

    private func receivePartial(_ text: String, generation: Int) {
        guard generation == self.generation, phase == .recording || phase == .finishing, !text.isEmpty else { return }
        lastPartial = text
        // 途中結果は未確定文字列として入力先に見せる。見せられないとき（文節変換中など）だけ小窓に出す
        if controller?.showVoicePartial(text) == true {
            partialShownInPlace = true
            return
        }
        let shown = text.count > Self.partialLength ? "…" + text.suffix(Self.partialLength) : text
        TapInputFeedback.shared.show((phase == .recording ? "● " : "… ") + shown, duration: Self.maximumHold + 5)
    }

    private func release() {
        switch phase {
        case .idle, .finishing:
            return
        case .waiting:
            // 録音を始める前に離した（キーを単独で軽く押しただけ）
            reset()
        case .recording:
            guard Self.now() - pressedAt >= Self.minimumHold else {
                cancel()
                return
            }
            finish()
        }
    }

    private func finish() {
        phase = .finishing
        let generation = self.generation
        TapInputFeedback.shared.show(
            lastPartial.isEmpty || partialShownInPlace
                ? "… 認識しています" : "… " + String(lastPartial.suffix(Self.partialLength)),
            duration: 10)
        guard #available(macOS 26.0, *), let session = session as? VoiceSession else {
            reset()
            return
        }
        let startTask = self.startTask
        Task {
            let result: Result<String, Error>
            do {
                try await startTask?.value
                result = .success(try await session.finish())
            } catch {
                await session.cancel()
                result = .failure(error)
            }
            await MainActor.run { self.deliver(result, generation: generation) }
        }
    }

    private func deliver(_ result: Result<String, Error>, generation: Int) {
        guard generation == self.generation, phase == .finishing else { return }
        let controller = self.controller
        // 見せている途中結果は消さずに残す（成功なら、入れるときに置き換わる）
        reset(clearPartial: false)
        switch result {
        case .success(let text):
            let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !trimmed.isEmpty else {
                controller?.clearVoicePartial()
                TapInputFeedback.shared.show("こえ: 聞き取れませんでした", duration: 1.5)
                return
            }
            guard let controller else {
                // iroha に切り替えたが、入力を受け持つコントローラが現れなかった
                TapInputFeedback.shared.show("こえ: 入れ先が見つかりませんでした（\(trimmed)）", duration: 5)
                return
            }
            controller.insertVoiceTranscript(trimmed)
        case .failure(let error):
            controller?.clearVoicePartial()
            let reason = (error as? VoiceRecognitionError)?.description ?? error.localizedDescription
            TapInputFeedback.shared.show("こえ: \(reason)", duration: 3)
        }
    }

    /// 録音をやめて結果を捨てる
    private func cancel() {
        if #available(macOS 26.0, *), let session = session as? VoiceSession {
            let startTask = self.startTask
            Task {
                _ = try? await startTask?.value
                await session.cancel()
            }
        }
        reset()
    }

    /// 待ち・録音の状態を捨てて小窓を閉じる（録音そのものの後始末は呼び出し側で済ませる）
    private func reset(clearPartial: Bool = true) {
        if clearPartial { controller?.clearVoicePartial() }
        partialShownInPlace = false
        if activeController == nil || !VoiceInputSettings.isEnabled { stopPolling() }
        generation += 1
        phase = .idle
        session = nil
        startTask = nil
        controller = nil
        switchesInputSource = false
        switchedAt = nil
        lastPartial = ""
        TapInputFeedback.shared.hide()
    }

    // MARK: キーの状態を読む

    private func startPolling() {
        guard pollTimer == nil else { return }
        let timer = Timer(timeInterval: Self.pollInterval, repeats: true) { [weak self] _ in self?.poll() }
        RunLoop.main.add(timer, forMode: .common)
        pollTimer = timer
    }

    private func stopPolling() {
        pollTimer?.invalidate()
        pollTimer = nil
    }

    private func poll() {
        switch phase {
        case .idle:
            pollWhileIdle()
        case .waiting, .recording:
            pollWhileHeld()
        case .finishing:
            break
        }
    }

    /// 押していないとき: 決めたキーだけが押された瞬間を拾う
    private func pollWhileIdle() {
        guard VoiceInputSettings.isEnabled, let controller = activeController else {
            stopPolling()
            return
        }
        let key = VoiceInputSettings.holdKey
        let down = key.isDown
        defer { wasDown = down }
        // ほかの修飾キーと一緒に押したとき（ショートカット）は始めない
        guard down, !wasDown, !key.otherModifiersDown else { return }
        press(key, controller: controller)
    }

    /// 押している間: 離したこと・ほかの入力を見る
    private func pollWhileHeld() {
        let held = Self.now() - pressedAt
        // 離したことが `flagsChanged` で届かなかった場合（押している間に入力先が変わったなど）
        if !holdKey.isDown {
            release()
            return
        }
        // 押したあとに他のキー・クリックがあった: ショートカットに使っている（⌘ つきのキーは入力メソッドに届かないことがある）
        for type in [CGEventType.keyDown, .leftMouseDown, .rightMouseDown, .otherMouseDown] {
            if CGEventSource.secondsSinceLastEventType(.combinedSessionState, eventType: type) < held {
                cancel()
                return
            }
        }
        if phase == .recording, held >= Self.maximumHold { finish() }
    }

    private static func now() -> Double { ProcessInfo.processInfo.systemUptime }
}

/// iroha の入力ソース（「こえ」モードで iroha 以外の入力ソースから切り替えるため）
enum IrohaInputSource {
    static let japaneseID = "dev.iroha.inputmethod.iroha.Japanese"

    /// 今の入力ソースが iroha（ひらがな・英数のどちらでも）か
    static var isCurrent: Bool {
        let current = TISCopyCurrentKeyboardInputSource().takeRetainedValue()
        return stringProperty(current, kTISPropertyBundleID) == Bundle.main.bundleIdentifier
    }

    /// iroha のひらがなに切り替える。切り替えられたら true
    static func selectJapanese() -> Bool {
        let filter = [kTISPropertyInputSourceID as String: japaneseID] as CFDictionary
        guard let sources = TISCreateInputSourceList(filter, false)?.takeRetainedValue() as? [TISInputSource],
              let source = sources.first else { return false }
        return TISSelectInputSource(source) == noErr
    }

    private static func stringProperty(_ source: TISInputSource, _ key: CFString) -> String? {
        guard let pointer = TISGetInputSourceProperty(source, key) else { return nil }
        return Unmanaged<CFString>.fromOpaque(pointer).takeUnretainedValue() as String
    }
}
