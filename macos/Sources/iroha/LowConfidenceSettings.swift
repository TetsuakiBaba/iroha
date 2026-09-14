import AppKit
import Foundation

/// 誤変換らしい箇所の強調（試験機能）の設定。
///
/// ライブ変換の未確定文字列のうち、モデルが同音異義語で迷った文字（読み制約を満たす1位と2位の
/// 対数確率差＝マージンが閾値未満）の下線を太いオレンジにする。既定OFF。
/// AJIMEE-Bench（zenz-small）ではマージン1.0で誤変換文の62%を検出、正しい文の18%に誤警報、
/// 光る文字は全体の1%（`iroha-cli confidence` で再計測できる）
enum LowConfidenceSettings {
    static let enabledKey = "lowConfidenceHighlight"
    static let sensitivityKey = "lowConfidenceSensitivity"

    /// 感度（強いほど多く光る）。値は UserDefaults に保存する rawValue
    enum Sensitivity: String, CaseIterable, Identifiable {
        case low, medium, high

        var id: String { rawValue }

        var label: String {
            switch self {
            case .low: return "弱"
            case .medium: return "標準"
            case .high: return "強"
            }
        }

        /// この値未満のマージンの文字を強調する（nat）
        var marginThreshold: Float {
            switch self {
            case .low: return 0.5
            case .medium: return 1.0
            case .high: return 2.0
            }
        }
    }

    static var isEnabled: Bool {
        UserDefaults.standard.bool(forKey: enabledKey)
    }

    static var sensitivity: Sensitivity {
        Sensitivity(rawValue: UserDefaults.standard.string(forKey: sensitivityKey) ?? "") ?? .medium
    }

    /// 強調する文字の下線の色
    static let underlineColor = NSColor.systemOrange
}
