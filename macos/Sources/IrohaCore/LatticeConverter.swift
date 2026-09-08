import Foundation
import KanaKanjiConverterModule

/// 辞書ラティス（AzooKeyKanaKanjiConverter）によるかな漢字変換。
///
/// zenz（LLM）は自由生成なので、読みの合わない語（「ないようを」→「活用を」）を
/// 候補に出してしまうことがある。辞書ラティスは「読みが本当に一致する語の組み合わせ」しか
/// 作れないので、ここで作った候補をzenzで並べ替えれば読みを保証したまま文脈を効かせられる
/// （`LatticeRescoringEngine`）。azooKey/Zenzaiと同じ役割分担
///
/// 辞書データは azooKey_dictionary_storage（Apache-2.0）。アプリでは
/// `iroha.app/Contents/Resources/Dictionary`、開発時は `vendor/azooKey_dictionary_storage/Dictionary`
public actor LatticeConverter {

    /// ラティスが返した1候補
    public struct Candidate: Sendable, Equatable {
        public let text: String
        /// ラティスの評価値（大きいほど良い。対数確率に相当するスケール）
        public let value: Double
        /// 読み全体を変換した候補か（false なら先頭文節だけ・予測などの部分候補）
        public let isFullMatch: Bool
    }

    /// 辞書フォルダの既定の探索先。見つからなければnil（ラティスなしで動く）
    ///
    /// 1. 環境変数 `IROHA_DICTIONARY`
    /// 2. アプリバンドルの `Contents/Resources/Dictionary`
    /// 3. カレントディレクトリから上へたどった `vendor/azooKey_dictionary_storage/Dictionary`
    ///    （iroha-cli・テストを `macos/` やリポジトリルートから実行したとき）
    public static func defaultDictionaryURL() -> URL? {
        var candidates: [URL] = []
        if let path = ProcessInfo.processInfo.environment["IROHA_DICTIONARY"], !path.isEmpty {
            candidates.append(URL(fileURLWithPath: path, isDirectory: true))
        }
        if let resources = Bundle.main.resourceURL {
            candidates.append(resources.appendingPathComponent("Dictionary", isDirectory: true))
        }
        var directory = URL(fileURLWithPath: FileManager.default.currentDirectoryPath, isDirectory: true)
        for _ in 0..<4 {
            candidates.append(
                directory.appendingPathComponent("vendor/azooKey_dictionary_storage/Dictionary", isDirectory: true))
            directory.deleteLastPathComponent()
        }
        return candidates.first { isDictionaryDirectory($0) }
    }

    /// 辞書フォルダとして使えるか（必須ファイルの有無で判定）
    public static func isDictionaryDirectory(_ url: URL) -> Bool {
        FileManager.default.fileExists(atPath: url.appendingPathComponent("mm.binary").path)
            && FileManager.default.fileExists(atPath: url.appendingPathComponent("louds/charID.chid").path)
    }

    private let converter: KanaKanjiConverter
    private let workDirectory: URL

    public init(dictionaryURL: URL) {
        converter = KanaKanjiConverter(dictionaryURL: dictionaryURL)
        // 学習は使わない（irohaは自前の学習を持つ）が、APIが保存先を要求するので一時フォルダを渡す
        workDirectory = FileManager.default.temporaryDirectory
            .appendingPathComponent("iroha-lattice", isDirectory: true)
    }

    /// 読み全体に一致する候補（ラティスの評価順）。最大 `count` 件
    public func candidates(reading: String, count: Int) -> [String] {
        rawCandidates(reading: reading, count: count)
            .filter(\.isFullMatch)
            .prefix(count)
            .map(\.text)
    }

    /// ラティスの返した候補をそのまま（部分候補も含む。調査・デバッグ用）
    public func rawCandidates(reading: String, count: Int) -> [Candidate] {
        guard !reading.isEmpty else { return [] }
        var composing = ComposingText()
        composing.insertAtCursorPosition(reading, inputStyle: .direct)
        let options = ConvertRequestOptions(
            N_best: max(count, 5),
            requireJapanesePrediction: false,
            requireEnglishPrediction: false,
            keyboardLanguage: .ja_JP,
            learningType: .nothing,
            memoryDirectoryURL: workDirectory,
            sharedContainerURL: workDirectory,
            textReplacer: TextReplacer(),
            specialCandidateProviders: [],
            metadata: nil)
        let result = converter.requestCandidates(composing, options: options)
        // 読みは独立して与えるので、差分変換用の内部状態は毎回捨てる
        converter.stopComposition()
        let readingLength = reading.count
        return result.mainResults.map {
            Candidate(text: $0.text, value: Double($0.value), isFullMatch: $0.rubyCount == readingLength)
        }
    }
}
