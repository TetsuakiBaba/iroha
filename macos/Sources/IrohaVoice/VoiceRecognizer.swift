import AVFoundation
import Foundation
import IrohaCore
import IrohaDocuments
import Speech

/// 「こえ」モードの音声認識（macOS 26 の SpeechAnalyzer。端末内で動く。2026-11-10）。
///
/// 認識器は 2 種類から選べる（2026-11-10 に読み上げ音声で比べた）:
/// - `speech`（SpeechTranscriber）: 新しいモデル。速い。句点・疑問符は文が終わったと見たときだけ付き、読点はほぼ付かない。
///   英字・数字の前に空白を入れる（`VoiceTranscript` が除く）。途中結果は `.fastResults` を付けないと発話中に届かない
///   （`.volatileResults` だけでは、入力を締めたあとにまとめて届いた。付けると約 1 秒ごと。最終結果は同じ）
/// - `dictation`（DictationTranscriber）: 以前からの音声入力と同じ系統。読点が付く。
///   音声の終わりで最後の 1 拍が落ちることがあったので、終わりに無音を足してから締める
public enum VoiceRecognizerKind: String, CaseIterable, Sendable {
    case speech
    case dictation
}

public enum VoiceRecognitionError: Error, CustomStringConvertible {
    case unsupportedOS
    case unsupportedLocale
    case assetsNotInstalled
    case noInputDevice
    case audioFormatUnavailable

    public var description: String {
        switch self {
        case .unsupportedOS: return "macOS 26 以降が必要です"
        case .unsupportedLocale: return "この Mac の音声認識は日本語に対応していません"
        case .assetsNotInstalled: return "音声認識のデータが入っていません"
        case .noInputDevice: return "マイクが見つかりません"
        case .audioFormatUnavailable: return "音声の形式を決められません"
        }
    }
}

/// 音声認識のデータ（OS が管理する。アプリには含めない）の状態
public enum VoiceAssetStatus: Sendable, Equatable {
    /// OS が古い・日本語に対応していない
    case unsupported
    /// 対応しているがデータが入っていない（`VoiceRecognition.installAssets` で入れる）
    case notInstalled
    case downloading
    case installed
}

public enum VoiceRecognition {
    public static let locale = Locale(identifier: "ja-JP")

    /// この OS で使えるか（macOS 26 以降）
    public static var isSupported: Bool {
        if #available(macOS 26.0, *) { return true }
        return false
    }

    /// 音声認識のデータの状態。
    ///
    /// OS の状態（`AssetInventory.status`）はアプリごとで、データが Mac に入っていても、そのアプリが使う言語として
    /// 取り置くまでは「入っていない」と返る（2026-11-10 実測。別の実行ファイルから見ると `.supported`）。
    /// Mac に入っていれば取り置きはすぐ済むので、ここでは「入っている」と答え、使うときに取り置く（`reserveAssets`）
    public static func assetStatus(kind: VoiceRecognizerKind) async -> VoiceAssetStatus {
        guard #available(macOS 26.0, *) else { return .unsupported }
        switch await AssetInventory.status(forModules: [makeModule(kind: kind, volatile: false)]) {
        case .unsupported: return .unsupported
        case .supported: return await isInstalledOnSystem(kind: kind) ? .installed : .notInstalled
        case .downloading: return .downloading
        case .installed: return .installed
        @unknown default: return .unsupported
        }
    }

    @available(macOS 26.0, *)
    private static func isInstalledOnSystem(kind: VoiceRecognizerKind) async -> Bool {
        let installed: [Locale]
        switch kind {
        case .speech: installed = await SpeechTranscriber.installedLocales
        case .dictation: installed = await DictationTranscriber.installedLocales
        }
        let wanted = locale.identifier(.bcp47)
        return installed.contains { $0.identifier(.bcp47) == wanted }
    }

    /// Mac に入っているデータを、このアプリが使う言語として取り置く。入っていなければ例外
    /// （ダウンロードは時間がかかるので、録音の途中では始めない。設定画面の `installAssets` で入れる）
    @available(macOS 26.0, *)
    static func reserveAssets(kind: VoiceRecognizerKind, module: any SpeechModule) async throws {
        let status = await AssetInventory.status(forModules: [module])
        if status == .installed { return }
        guard status == .supported, await isInstalledOnSystem(kind: kind) else {
            throw status == .unsupported
                ? VoiceRecognitionError.unsupportedLocale : VoiceRecognitionError.assetsNotInstalled
        }
        if let request = try await AssetInventory.assetInstallationRequest(supporting: [module]) {
            try await request.downloadAndInstall()
        }
    }

    /// 音声認識のデータを入れる（入っていれば何もしない）。`progress` は 0〜1
    public static func installAssets(
        kind: VoiceRecognizerKind, progress: (@Sendable (Double) -> Void)? = nil
    ) async throws {
        guard #available(macOS 26.0, *) else { throw VoiceRecognitionError.unsupportedOS }
        let module = makeModule(kind: kind, volatile: false)
        guard let request = try await AssetInventory.assetInstallationRequest(supporting: [module]) else { return }
        let observation = progress.map { progress in
            request.progress.observe(\.fractionCompleted) { value, _ in progress(value.fractionCompleted) }
        }
        defer { observation?.invalidate() }
        try await request.downloadAndInstall()
    }

    /// 音声ファイルを認識する（iroha-cli の確認用）
    public static func transcribe(file url: URL, kind: VoiceRecognizerKind) async throws -> String {
        guard #available(macOS 26.0, *) else { throw VoiceRecognitionError.unsupportedOS }
        let module = makeModule(kind: kind, volatile: false)
        try await reserveAssets(kind: kind, module: module)
        let file = try AVAudioFile(forReading: url)
        let analyzer = SpeechAnalyzer(modules: [module])
        let collector = ResultCollector(module: module, onPartial: nil)
        if let last = try await analyzer.analyzeSequence(from: file) {
            try await analyzer.finalizeAndFinish(through: last)
        } else {
            await analyzer.cancelAndFinishNow()
        }
        return await collector.finish()
    }

    /// 音声ファイルを、マイクで録ったときと同じ経路（録った順に少しずつ渡す）で認識する（iroha-cli の確認用）。
    /// 実時間の速さで渡し、途中結果が届くたびに `onPartial` を呼ぶ
    public static func transcribeStreaming(
        file url: URL, kind: VoiceRecognizerKind, onPartial: @escaping @Sendable (String) -> Void
    ) async throws -> String {
        guard #available(macOS 26.0, *) else { throw VoiceRecognitionError.unsupportedOS }
        let file = try AVAudioFile(forReading: url)
        let inputFormat = file.processingFormat
        let module = makeModule(kind: kind, volatile: true)
        try await reserveAssets(kind: kind, module: module)
        guard let format = await SpeechAnalyzer.bestAvailableAudioFormat(
            compatibleWith: [module], considering: inputFormat)
        else { throw VoiceRecognitionError.audioFormatUnavailable }
        let analyzer = SpeechAnalyzer(modules: [module])
        let collector = ResultCollector(module: module, onPartial: onPartial)
        let (stream, continuation) = AsyncStream<AnalyzerInput>.makeStream()
        try await analyzer.start(inputSequence: stream)
        let feeder = AudioFeeder()
        feeder.connect(to: continuation, inputFormat: inputFormat, outputFormat: format)
        let chunk = AVAudioFrameCount(inputFormat.sampleRate / 10)
        while file.framePosition < file.length {
            guard let buffer = AVAudioPCMBuffer(pcmFormat: inputFormat, frameCapacity: chunk) else { break }
            try file.read(into: buffer, frameCount: chunk)
            if buffer.frameLength == 0 { break }
            feeder.receive(buffer)
            try await Task.sleep(for: .milliseconds(100))
        }
        feeder.close(trailingSilence: 0.4)
        try await analyzer.finalizeAndFinishThroughEndOfInput()
        return await collector.finish()
    }

    /// 認識結果を読みと固定部分に分ける（分かち書きと読みは macOS の `CFStringTokenizer`）
    public static func transcript(from text: String, comma: String = "、", period: String = "。") -> VoiceTranscript {
        VoiceTranscript(tokens: SystemReadingTokenizer().tokens(in: text), comma: comma, period: period)
    }

    @available(macOS 26.0, *)
    static func makeModule(kind: VoiceRecognizerKind, volatile: Bool) -> any SpeechModule {
        switch kind {
        case .speech:
            return SpeechTranscriber(
                locale: locale, transcriptionOptions: [],
                reportingOptions: volatile ? [.volatileResults, .fastResults] : [], attributeOptions: [])
        case .dictation:
            return DictationTranscriber(
                locale: locale, contentHints: [], transcriptionOptions: [.punctuation],
                reportingOptions: volatile ? [.volatileResults] : [], attributeOptions: [])
        }
    }
}

/// 認識器の結果を受けて、確定した部分と途中の部分をつなげて持つ
@available(macOS 26.0, *)
final class ResultCollector: @unchecked Sendable {
    private let lock = NSLock()
    private var finalized = ""
    private var volatile = ""
    private var task: Task<Void, Never>?

    init(module: any SpeechModule, onPartial: (@Sendable (String) -> Void)?) {
        task = Task { [weak self] in
            do {
                if let transcriber = module as? SpeechTranscriber {
                    for try await result in transcriber.results {
                        self?.receive(String(result.text.characters), isFinal: result.isFinal, onPartial: onPartial)
                    }
                } else if let transcriber = module as? DictationTranscriber {
                    for try await result in transcriber.results {
                        self?.receive(String(result.text.characters), isFinal: result.isFinal, onPartial: onPartial)
                    }
                }
            } catch {
                // 取り消し・認識器のエラー: それまでに届いた分を結果にする
            }
        }
    }

    private func receive(_ text: String, isFinal: Bool, onPartial: (@Sendable (String) -> Void)?) {
        lock.lock()
        if isFinal {
            finalized += text
            volatile = ""
        } else {
            volatile = text
        }
        let current = finalized + volatile
        lock.unlock()
        onPartial?(current)
    }

    /// 認識器が結果を出し終えるのを待って、全体を返す
    func finish() async -> String {
        await task?.value
        return lock.withLock { finalized + volatile }
    }

    func cancel() {
        task?.cancel()
    }
}
