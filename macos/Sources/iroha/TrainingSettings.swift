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
    static let epochsRange = 1...20

    /// 学習率（既定 1e-4）。自由に入力できるが `learningRateRange` に収める。`learningRateChoices` はプリセット
    static let learningRateKey = "trainingLearningRate"
    static let defaultLearningRate = 1e-4
    static let learningRateRange = 1e-6...1e-2
    static let learningRateChoices: [(label: String, value: Double)] = [
        ("弱め 5e-5", 5e-5), ("標準 1e-4", 1e-4), ("強め 2e-4", 2e-4), ("かなり強め 5e-4", 5e-4),
    ]

    static var epochs: Int {
        let value = UserDefaults.standard.integer(forKey: epochsKey)
        return epochsRange.contains(value) ? value : defaultEpochs
    }

    static var learningRate: Double {
        let value = UserDefaults.standard.double(forKey: learningRateKey)
        return learningRateRange.contains(value) ? value : defaultLearningRate
    }

    /// 学習率の表示（1e-4 のような指数表記。ユーザが入力する形と揃える）
    static func format(learningRate value: Double) -> String {
        String(format: "%g", value)
    }

    /// ユーザの入力を学習率として解釈する（"1e-4" / "0.0001" どちらも可。範囲外・解釈不能は nil）
    static func parse(learningRate text: String) -> Double? {
        guard let value = Double(text.trimmingCharacters(in: .whitespaces)), learningRateRange.contains(value) else {
            return nil
        }
        return value
    }
}
