import Foundation
import IrohaMLX

/// かな漢字変換のニューラルネットを動かす仕組み（設定 > モデル > 推論エンジン）。
///
/// 既定は MLX が使える Mac（Apple Silicon）では MLX、それ以外では llama.cpp（`defaultBackend`。2026-11-15 に
/// llama.cpp から変更）。MLX（`MLXConversionEngine`）は T5 のモデルにだけ対応していて、デコーダが 2 層の T5 では
/// llama.cpp より速い（iroha-kkc-v1 Q5_K_M・Apple M4 Pro・iroha-bench-v1 全 3,253 問で、変換 1 回の中央値が
/// 14.6ms → 12.1ms。正解は 2,248 → 2,244 問で、計算が bf16 のため数問だけ変わる）。
/// zenz（GPT-2、12 層）では MLX のほうが遅いので対応していない（`resolve` が llama.cpp に戻す）。
/// MLX は重みをメモリに持つので、プロセスの使用量は llama.cpp より大きい（AJIMEE 中のピーク 80MB → 447MB）。
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

    /// 設定で選ばれているもの（選んでいなければ `defaultBackend`）
    static var selected: InferenceBackend {
        UserDefaults.standard.string(forKey: userDefaultsKey).flatMap(InferenceBackend.init(rawValue:)) ?? defaultBackend
    }

    /// 選んでいないときに使うもの。MLX が動く Mac なら MLX（T5 以外のモデルでは `resolve` が llama.cpp に戻す）
    static var defaultBackend: InferenceBackend {
        isMLXSupportedHardware ? .mlx : .llamaCpp
    }

    /// 設定画面で推論エンジンを選んだことがあるか（既定のままなら偽）
    static var isExplicitlySelected: Bool {
        UserDefaults.standard.string(forKey: userDefaultsKey) != nil
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
