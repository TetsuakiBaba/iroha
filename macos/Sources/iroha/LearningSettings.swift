import Foundation
import IrohaCore

/// 変換の学習まわりの設定。
enum LearningSettings {

    /// ユーザの修正を学習するか（既定ON）
    static let enabledKey = "learningEnabled"

    static var isEnabled: Bool {
        UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? true
    }

    /// いま学習を使うか。設定がONでも、プライベートモードの間は使わない（`PrivateModeSettings`）
    static var isActive: Bool {
        isEnabled && !PrivateModeSettings.isEnabled
    }

    /// 学習を使わない間は参照もしない（記録も適用も止める）
    static var dictionary: LearningDictionary {
        isActive ? LearningStore.shared.current : .empty
    }
}
