import AVFoundation
import Foundation
import Speech

/// マイクの許可（OS の「プライバシーとセキュリティ > マイク」）
public enum VoiceMicrophone {
    public enum Status: Sendable, Equatable {
        case notDetermined
        case denied
        case authorized
    }

    public static var status: Status {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized: return .authorized
        case .notDetermined: return .notDetermined
        default: return .denied
        }
    }

    /// 許可のダイアログを出す。入力の途中では呼ばない（設定画面から呼ぶ。ダイアログに答えるまでの間、
    /// 入力メソッドとしての応答を止めないため）
    public static func requestAccess() async -> Bool {
        await AVCaptureDevice.requestAccess(for: .audio)
    }
}

/// マイクから録音しながら認識する 1 回ぶん（キーを押してから離すまで）。
///
/// `start()` はまず録音を始め、認識器の準備はその後に進める（準備の間に話した分は取っておいて後から渡す。
/// 押してすぐ話し始めても語頭が落ちないようにするため）。`finish()` で録音を止めて、認識結果の全体を返す
@available(macOS 26.0, *)
public final class VoiceSession: @unchecked Sendable {
    private let kind: VoiceRecognizerKind
    private let onPartial: @Sendable (String) -> Void
    private let engine = AVAudioEngine()
    private let feeder = AudioFeeder()
    private var analyzer: SpeechAnalyzer?
    private var collector: ResultCollector?
    private var prepareTask: Task<Void, Error>?
    private var tapInstalled = false

    /// 認識器を使い終わってもしばらくモデルを残す（次に押したときの立ち上がりを速くする）
    private static let options = SpeechAnalyzer.Options(priority: .userInitiated, modelRetention: .lingering)
    /// 終わりに足す無音（DictationTranscriber は音声の終わりで最後の 1 拍を落とすことがある）
    private static let trailingSilence = 0.4

    /// - Parameter onPartial: 途中の認識結果（確定した分 + 途中の分）。呼ばれるスレッドは決まっていない
    public init(kind: VoiceRecognizerKind, onPartial: @escaping @Sendable (String) -> Void) {
        self.kind = kind
        self.onPartial = onPartial
    }

    /// 録音を始める。マイクが使えなければ例外
    public func start() throws {
        let input = engine.inputNode
        let inputFormat = input.outputFormat(forBus: 0)
        guard inputFormat.sampleRate > 0, inputFormat.channelCount > 0 else {
            throw VoiceRecognitionError.noInputDevice
        }
        let feeder = self.feeder
        input.installTap(onBus: 0, bufferSize: 2048, format: inputFormat) { buffer, _ in
            feeder.receive(buffer)
        }
        tapInstalled = true
        engine.prepare()
        do {
            try engine.start()
        } catch {
            stopAudio()
            throw error
        }

        let kind = self.kind
        let onPartial = self.onPartial
        prepareTask = Task { [weak self] in
            let module = VoiceRecognition.makeModule(kind: kind, volatile: true)
            try await VoiceRecognition.reserveAssets(kind: kind, module: module)
            guard let format = await SpeechAnalyzer.bestAvailableAudioFormat(
                compatibleWith: [module], considering: inputFormat)
            else { throw VoiceRecognitionError.audioFormatUnavailable }
            let analyzer = SpeechAnalyzer(modules: [module], options: Self.options)
            let collector = ResultCollector(module: module, onPartial: onPartial)
            let (stream, continuation) = AsyncStream<AnalyzerInput>.makeStream()
            try await analyzer.start(inputSequence: stream)
            self?.analyzer = analyzer
            self?.collector = collector
            // ここまでに録った分を渡し、以後は録ったそばから渡す
            feeder.connect(to: continuation, inputFormat: inputFormat, outputFormat: format)
        }
    }

    /// 録音を止めて、認識結果の全体を返す（準備に失敗していたら例外）
    public func finish() async throws -> String {
        stopAudio()
        do {
            try await prepareTask?.value
        } catch {
            feeder.close(trailingSilence: 0)
            throw error
        }
        feeder.close(trailingSilence: Self.trailingSilence)
        try await analyzer?.finalizeAndFinishThroughEndOfInput()
        return await collector?.finish() ?? ""
    }

    /// 録音をやめて結果を捨てる
    public func cancel() async {
        stopAudio()
        prepareTask?.cancel()
        _ = try? await prepareTask?.value
        feeder.close(trailingSilence: 0)
        collector?.cancel()
        await analyzer?.cancelAndFinishNow()
    }

    private func stopAudio() {
        if tapInstalled {
            engine.inputNode.removeTap(onBus: 0)
            tapInstalled = false
        }
        if engine.isRunning { engine.stop() }
    }
}

/// 録った音声を認識器の形式に直して渡す。認識器の準備が済むまでは取っておく。
/// 録音のスレッド（`receive`）と準備・終了のスレッドから触るのでロックで守る
@available(macOS 26.0, *)
final class AudioFeeder: @unchecked Sendable {
    private let lock = NSLock()
    private var pending: [AVAudioPCMBuffer] = []
    private var continuation: AsyncStream<AnalyzerInput>.Continuation?
    private var converter: AVAudioConverter?
    private var outputFormat: AVAudioFormat?
    private var closed = false

    func receive(_ buffer: AVAudioPCMBuffer) {
        lock.lock()
        defer { lock.unlock() }
        guard !closed else { return }
        if continuation == nil {
            // 録音のバッファは使い回されるので、取っておく分は写しを作る
            if let copy = Self.copy(buffer) { pending.append(copy) }
            return
        }
        yield(buffer)
    }

    func connect(
        to continuation: AsyncStream<AnalyzerInput>.Continuation, inputFormat: AVAudioFormat,
        outputFormat: AVAudioFormat
    ) {
        lock.lock()
        defer { lock.unlock() }
        guard !closed else {
            continuation.finish()
            return
        }
        self.continuation = continuation
        self.outputFormat = outputFormat
        converter = inputFormat == outputFormat ? nil : AVAudioConverter(from: inputFormat, to: outputFormat)
        // 音声入力では頭の音を捨てない（既定では最初の数ミリ秒ぶんを変換器が抱える）
        converter?.primeMethod = .none
        for buffer in pending { yield(buffer) }
        pending = []
    }

    /// 無音を足して入力を締める（準備が済んでいなければ、取っておいた分を捨てる）
    func close(trailingSilence seconds: Double) {
        lock.lock()
        defer { lock.unlock() }
        guard !closed else { return }
        closed = true
        pending = []
        guard let continuation else { return }
        if seconds > 0, let outputFormat,
           let silence = AVAudioPCMBuffer(
            pcmFormat: outputFormat, frameCapacity: AVAudioFrameCount(outputFormat.sampleRate * seconds)) {
            // 確保した直後のバッファは 0 で埋まっている
            silence.frameLength = silence.frameCapacity
            continuation.yield(AnalyzerInput(buffer: silence))
        }
        continuation.finish()
    }

    /// ロックを持ったまま呼ぶ
    private func yield(_ buffer: AVAudioPCMBuffer) {
        guard let continuation, let outputFormat else { return }
        guard let converter else {
            if let copy = Self.copy(buffer) { continuation.yield(AnalyzerInput(buffer: copy)) }
            return
        }
        let ratio = outputFormat.sampleRate / buffer.format.sampleRate
        let capacity = AVAudioFrameCount((Double(buffer.frameLength) * ratio).rounded(.up)) + 16
        guard let output = AVAudioPCMBuffer(pcmFormat: outputFormat, frameCapacity: capacity) else { return }
        var supplied = false
        var error: NSError?
        let status = converter.convert(to: output, error: &error) { _, inputStatus in
            if supplied {
                inputStatus.pointee = .noDataNow
                return nil
            }
            supplied = true
            inputStatus.pointee = .haveData
            return buffer
        }
        guard status != .error, output.frameLength > 0 else { return }
        continuation.yield(AnalyzerInput(buffer: output))
    }

    private static func copy(_ buffer: AVAudioPCMBuffer) -> AVAudioPCMBuffer? {
        guard let copy = AVAudioPCMBuffer(pcmFormat: buffer.format, frameCapacity: buffer.frameLength) else {
            return nil
        }
        copy.frameLength = buffer.frameLength
        let source = UnsafeMutableAudioBufferListPointer(buffer.mutableAudioBufferList)
        let destination = UnsafeMutableAudioBufferListPointer(copy.mutableAudioBufferList)
        for (from, to) in zip(source, destination) {
            guard let fromData = from.mData, let toData = to.mData else { continue }
            memcpy(toData, fromData, Int(min(from.mDataByteSize, to.mDataByteSize)))
        }
        return copy
    }
}
