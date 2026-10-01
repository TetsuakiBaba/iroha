import Foundation
import IrohaMLX

/// かな漢字変換のニューラルネットを動かす仕組み（設定 > モデル > 推論エンジン）。
///
/// 既定は llama.cpp（`ZenzEngine`）。MLX（`MLXConversionEngine`）は T5 のモデルにだけ対応していて、
/// デコーダが 2 層の T5 では 1 回の変換が約 2 割速い（出力は同じ。`training/t5/MAC-AJIMEE-MLX-2026-10-01.md`）。
/// zenz（GPT-2、12 層）では MLX のほうが遅いので対応していない。
/// モデルのパスと同じく機械ごとの設定（`PreferencesSync` で同期しない）で、変更は再起動後に反映する
enum InferenceBackend: String, CaseIterable, Identifiable {
    case llamaCpp = "llama"
    case mlx = "mlx"

    static let userDefaultsKey = "inferenceBackend"

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .llamaCpp: return "llama.cpp"
        case .mlx: return "MLX"
        }
    }

    /// 設定で選ばれているもの
    static var selected: InferenceBackend {
        UserDefaults.standard.string(forKey: userDefaultsKey).flatMap(InferenceBackend.init(rawValue:)) ?? .llamaCpp
    }

    /// MLX が動く機械か（Apple Silicon のみ）
    static var isMLXSupportedHardware: Bool {
        #if arch(arm64)
        return true
        #else
        return false
        #endif
    }

    /// 実際に使うもの。MLX を選んでいても、MLX が読めないモデル（T5 以外・ファイルが無い）や
    /// Apple Silicon 以外では llama.cpp で動かす（変換できなくなるよりよい。設定画面で理由を出す）
    static func resolve(modelPath: String) -> InferenceBackend {
        guard selected == .mlx, isMLXSupportedHardware, MLXConversionEngine.supports(modelPath: modelPath) else {
            return .llamaCpp
        }
        return .mlx
    }
}
