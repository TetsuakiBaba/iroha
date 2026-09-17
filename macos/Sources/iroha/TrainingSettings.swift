import Foundation

/// 変換記録からの追加学習（`iroha-train`）に関する設定キー
enum TrainingSettings {
    /// 使用する LoRA アダプタ（GGUF）のパス。空なら使わない。`modelPath` と同じく再起動後に反映
    static let adapterPathKey = "modelAdapterPath"

    static var adapterPath: String? {
        let value = UserDefaults.standard.string(forKey: adapterPathKey) ?? ""
        return value.isEmpty ? nil : value
    }
}
