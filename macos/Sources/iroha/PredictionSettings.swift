import Foundation
import IrohaCore

/// 予測変換（確定前）・インライン補完（確定後）の設定。
///
/// - 予測変換: 入力中にキー入力が休止したら、未確定文字列の続き（次の文節）をカーソル下の小窓に出し、Tabで取り入れる
/// - インライン補完: 確定後にキー入力が休止したら、確定した文章の続きを同じ小窓に出し、Tabで確定する
///
/// どちらもかな漢字変換とは別のモデルを指定できる（`PredictionEngine`）。既定ではかな漢字変換と
/// 同じzenzモデルを共有する（zenz-v3は左文脈の続きも書けるため。モデルは1回しかロードしない）
enum PredictionSettings {

    static let predictiveEnabledKey = "predictiveConversion"
    static let completionEnabledKey = "inlineCompletion"
    /// 予測変換・インライン補完に使うモデル（GGUF）のパス。空ならかな漢字変換と同じモデル
    static let predictiveModelPathKey = "predictiveModelPath"
    static let completionModelPathKey = "completionModelPath"
    /// キー入力の休止とみなす時間（ミリ秒）
    static let delayMillisecondsKey = "predictionDelayMs"
    static let defaultDelayMilliseconds = 300
    static let delayMillisecondsRange = 100...2000

    /// 予測変換（既定OFF）
    static var isPredictiveEnabled: Bool {
        UserDefaults.standard.object(forKey: predictiveEnabledKey) as? Bool ?? false
    }

    /// インライン補完（既定OFF）
    static var isCompletionEnabled: Bool {
        UserDefaults.standard.object(forKey: completionEnabledKey) as? Bool ?? false
    }

    /// キー入力の休止時間（既定300ms、100〜2000msに丸める）
    static var idleDelay: Duration {
        let stored = UserDefaults.standard.integer(forKey: delayMillisecondsKey)
        let milliseconds = stored == 0 ? defaultDelayMilliseconds
            : min(delayMillisecondsRange.upperBound, max(delayMillisecondsRange.lowerBound, stored))
        return .milliseconds(milliseconds)
    }

    /// 予測として表示する最大文字数
    static let maxLength = 16

    /// 設定されたモデルのパス。空（未設定）なら `fallback`（かな漢字変換のモデル）
    static func resolvedModelPath(forKey key: String, fallback: String) -> String {
        if let path = UserDefaults.standard.string(forKey: key), !path.isEmpty {
            return path
        }
        return fallback
    }

    /// 設定のモデルに対応するエンジン。同じモデルファイルを指す既存のエンジンがあればそれを共有する
    /// （かな漢字変換のzenz、または予測変換のエンジンとの二重ロードを避ける）
    static func engine(
        forKey key: String, fallbackPath: String,
        sharing existing: [(path: String, engine: any PredictionEngine)]
    ) -> any PredictionEngine {
        let path = resolvedModelPath(forKey: key, fallback: fallbackPath)
        if let shared = existing.first(where: { $0.path == path }) {
            return shared.engine
        }
        return ZenzEngine(modelPath: path)
    }

    /// 表示用のモデル名（未設定なら「かな漢字変換と同じ」）
    static func modelDisplayName(forKey key: String) -> String {
        guard let path = UserDefaults.standard.string(forKey: key), !path.isEmpty else {
            return "かな漢字変換と同じモデル"
        }
        guard FileManager.default.fileExists(atPath: path) else { return "ファイルが見つかりません" }
        return URL(fileURLWithPath: path).deletingPathExtension().lastPathComponent
    }
}
