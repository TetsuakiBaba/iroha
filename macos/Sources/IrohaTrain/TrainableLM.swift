import Foundation
import MLX
import MLXNN
import IrohaCore

/// LoRA の掛け方
public struct LoRASpec: Sendable, Equatable {
    public var rank: Int
    public var alpha: Float
    /// 対象テンソル名の末尾（`blk.N.<名前>.weight`）
    public var targets: [String]

    public init(rank: Int, alpha: Float, targets: [String]) {
        self.rank = rank
        self.alpha = alpha
        self.targets = targets
    }

    public init(_ config: TrainingConfig) {
        self.init(rank: config.rank, alpha: config.alpha, targets: config.targets)
    }
}

public enum TrainableLMError: Error, CustomStringConvertible {
    case unsupportedArchitecture(String)
    case missingTensor(String)
    case missingMetadata(String)

    public var description: String {
        switch self {
        case .unsupportedArchitecture(let arch): return "このモデルのアーキテクチャは追加学習に対応していません: \(arch)"
        case .missingTensor(let name): return "モデルにテンソルがありません: \(name)"
        case .missingMetadata(let key): return "モデルのメタデータがありません: \(key)"
        }
    }
}

/// LoRA 層とベースのテンソル名の対応表。`Module` の反射（Mirror）はプロパティに Module / MLXArray を含む
/// 配列・タプルを辿ってパラメータとして数えるので、二重登録を避けるために Module でも配列でもない箱に入れる
public final class LoRARegistry {
    public private(set) var layers: [(baseName: String, layer: LoRALinear)] = []
    public init() {}
    public func register(_ baseName: String, _ layer: LoRALinear) { layers.append((baseName, layer)) }
}

/// MLX で forward/backward できる言語モデル。GGUF（f16/f32）から重みを読み、指定した線形層を LoRA に差し替える。
/// アーキテクチャを増やすときはこのプロトコルの実装を足し、`TrainableModels.load` に登録する
public protocol TrainableLM: Module {
    /// `general.architecture` の値
    static var architecture: String { get }
    init(gguf: GGUFFile, lora: LoRASpec?) throws
    /// トークン列 [B, T] → ロジット [B, T, V]（float32）
    func callAsFunction(_ tokens: MLXArray) -> MLXArray
    /// LoRA 層と対応するベースのテンソル名
    var lora: LoRARegistry { get }
}

public extension TrainableLM {
    /// 学習対象を LoRA だけにする（ベースの重みは凍結）
    func freezeBase() {
        freeze()
        unfreeze(keys: ["lora_a", "lora_b"])
    }

    /// 学習した LoRA を llama.cpp アダプタの形で取り出す
    func exportLoraPairs() throws -> [LoraAdapterWriter.Pair] {
        try lora.layers.map { try $0.layer.exportPair(baseName: $0.baseName) }
    }

    var loraLayers: [(baseName: String, layer: LoRALinear)] { lora.layers }
}

public enum TrainableModels {
    /// GGUF のアーキテクチャに合う実装でモデルを読む
    public static func load(gguf: GGUFFile, lora: LoRASpec?) throws -> any TrainableLM {
        switch gguf.architecture {
        case GPT2Model.architecture: return try GPT2Model(gguf: gguf, lora: lora)
        case let other: throw TrainableLMError.unsupportedArchitecture(other ?? "(不明)")
        }
    }

    public static let supportedArchitectures: [String] = [GPT2Model.architecture]
}

/// 重みの読み込み補助
enum GGUFWeights {
    /// テンソルを行優先の float32 配列として読む（GGUF ne=[in,out] → MLX shape [out,in] = `Linear.weight`）
    static func array(_ gguf: GGUFFile, _ name: String) throws -> MLXArray {
        guard let tensor = gguf.tensor(named: name) else { throw TrainableLMError.missingTensor(name) }
        return MLXArray(try gguf.floats(of: tensor), tensor.rowMajorShape)
    }

    static func optionalArray(_ gguf: GGUFFile, _ name: String) throws -> MLXArray? {
        gguf.tensor(named: name) == nil ? nil : try array(gguf, name)
    }

    static func linear(_ gguf: GGUFFile, _ prefix: String, lora: LoRASpec?) throws -> (layer: UnaryLayer, lora: LoRALinear?) {
        let linear = Linear(weight: try array(gguf, prefix + ".weight"), bias: try optionalArray(gguf, prefix + ".bias"))
        let leaf = prefix.split(separator: ".").last.map(String.init) ?? prefix
        if let lora, lora.targets.contains(leaf) {
            let wrapped = LoRALinear(base: linear, rank: lora.rank, alpha: lora.alpha)
            return (wrapped, wrapped)
        }
        return (linear, nil)
    }
}
