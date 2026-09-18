import Foundation

/// 変換記録からの追加学習（`iroha-train`）に関する設定キー
enum TrainingSettings {
    /// 使用する LoRA アダプタ（GGUF）のパス。空なら使わない。`modelPath` と同じく再起動後に反映
    static let adapterPathKey = "modelAdapterPath"

    static var adapterPath: String? {
        let value = UserDefaults.standard.string(forKey: adapterPathKey) ?? ""
        return value.isEmpty ? nil : value
    }

    /// 学習のエポック数（既定 3）。この Mac の GPU 向けの調整なので同期しない
    static let epochsKey = "trainingEpochs"
    static let defaultEpochs = 3
    static let epochsRange = 1...10

    /// 学習率（既定 1e-4）。選択肢は `learningRateChoices`
    static let learningRateKey = "trainingLearningRate"
    static let defaultLearningRate = 1e-4
    static let learningRateChoices: [(label: String, value: Double)] = [
        ("弱め（5e-5）", 5e-5), ("標準（1e-4）", 1e-4), ("強め（2e-4）", 2e-4),
    ]

    static var epochs: Int {
        let value = UserDefaults.standard.integer(forKey: epochsKey)
        return epochsRange.contains(value) ? value : defaultEpochs
    }

    static var learningRate: Double {
        let value = UserDefaults.standard.double(forKey: learningRateKey)
        return value > 0 ? value : defaultLearningRate
    }
}
