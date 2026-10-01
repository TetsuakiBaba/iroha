import Foundation
import CLlama
import MLX
import MLXNN
import IrohaCore

/// LoRA の掛け方
public struct LoRASpec: Sendable, Equatable {
    public var rank: Int
    public var alpha: Float
    /// 対象テンソル名の末尾（`blk.N.<名前>.weight`）。nil ならモデルの `defaultLoRATargets`
    public var targets: [String]?

    public init(rank: Int, alpha: Float, targets: [String]?) {
        self.rank = rank
        self.alpha = alpha
        self.targets = targets
    }

    public init(_ config: TrainingConfig) {
        self.init(rank: config.rank, alpha: config.alpha, targets: config.targets)
    }

    /// 対象が未指定なら `defaults` で埋める
    func resolved(defaults: [String]) -> LoRASpec {
        LoRASpec(rank: rank, alpha: alpha, targets: targets ?? defaults)
    }
}

/// 学習 1 バッチ分の配列（`LoRATrainer.makeBatch` が作る）
public struct TrainingBatch {
    /// デコーダの入力 [B, T]（= 各例の `tokens[:-1]` を右パディング）
    public let inputs: MLXArray
    /// 予測する正解 [B, T]（= `tokens[1:]`）
    public let targets: MLXArray
    /// 損失を掛ける位置 [B, T]（1 = 出力部と終端）
    public let mask: MLXArray
    /// エンコーダの入力 [B, S]。デコーダ専用モデルは nil
    public let source: MLXArray?
    /// `source` の有効位置 [B, S]（1 = 実トークン、0 = パディング）
    public let sourceMask: MLXArray?

    public init(inputs: MLXArray, targets: MLXArray, mask: MLXArray, source: MLXArray? = nil, sourceMask: MLXArray? = nil) {
        self.inputs = inputs
        self.targets = targets
        self.mask = mask
        self.source = source
        self.sourceMask = sourceMask
    }

    /// `valueAndGrad` に渡す形（配列のリスト）との相互変換
    var arrays: [MLXArray] {
        [inputs, targets, mask] + (source.map { [$0, sourceMask!] } ?? [])
    }

    init(arrays: [MLXArray]) {
        self.init(inputs: arrays[0], targets: arrays[1], mask: arrays[2],
                  source: arrays.count > 3 ? arrays[3] : nil, sourceMask: arrays.count > 4 ? arrays[4] : nil)
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
    /// `LoRASpec.targets` が未指定のときに LoRA を掛ける層（テンソル名の末尾）
    static var defaultLoRATargets: [String] { get }
    init(gguf: GGUFFile, lora: LoRASpec?) throws
    /// バッチ → デコーダ入力の各位置のロジット [B, T, V]（float32）
    func logits(_ batch: TrainingBatch) -> MLXArray
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
        case T5Model.architecture: return try T5Model(gguf: gguf, lora: lora)
        case let other: throw TrainableLMError.unsupportedArchitecture(other ?? "(不明)")
        }
    }

    public static let supportedArchitectures: [String] = [GPT2Model.architecture, T5Model.architecture]
}

/// 重みの読み込み補助
enum GGUFWeights {
    /// テンソルを行優先の float32 配列として読む（GGUF ne=[in,out] → MLX shape [out,in] = `Linear.weight`）
    static func array(_ gguf: GGUFFile, _ name: String) throws -> MLXArray {
        guard let tensor = gguf.tensor(named: name) else { throw TrainableLMError.missingTensor(name) }
        return MLXArray(try gguf.floats(of: tensor), tensor.rowMajorShape)
    }

    /// `dtype` を指定すると、その型の配列として読んですぐ評価する（推論用）。F16 のテンソルは f32 の配列を
    /// 経由しないので、読み込みの途中で全重みの f32 版がメモリに載ることがない（nil なら `array(_:_:)` と同じ f32）
    static func array(_ gguf: GGUFFile, _ name: String, dtype: DType?) throws -> MLXArray {
        guard let dtype else { return try array(gguf, name) }
        guard let tensor = gguf.tensor(named: name) else { throw TrainableLMError.missingTensor(name) }
        // ファイルから読んだ Data（FileHandle が返す NSData）は自動解放なので、ここで解放させる。
        // Swift の並行処理のスレッドでは自動解放プールがすぐには空にならず、全テンソルぶん（モデルと同じ大きさ）が
        // 読み込み後も残る（常駐する IME のメモリになる）
        return try autoreleasepool {
            let raw = tensor.type == GGML_TYPE_F16
                ? MLXArray(try gguf.data(of: tensor), tensor.rowMajorShape, dtype: .float16)
                : MLXArray(try gguf.floats(of: tensor), tensor.rowMajorShape)
            return converted(raw, to: dtype)
        }
    }

    private static func converted(_ array: MLXArray, to dtype: DType) -> MLXArray {
        let result = array.dtype == dtype ? array : array.asType(dtype)
        eval(result)
        return result
    }

    static func optionalArray(_ gguf: GGUFFile, _ name: String, dtype: DType? = nil) throws -> MLXArray? {
        gguf.tensor(named: name) == nil ? nil : try array(gguf, name, dtype: dtype)
    }

    static func linear(_ gguf: GGUFFile, _ prefix: String, lora: LoRASpec?, adapter: LoRAAdapterMerger? = nil,
                       dtype: DType? = nil) throws -> (layer: UnaryLayer, lora: LoRALinear?) {
        var weight: MLXArray
        if let adapter {
            // 足し込みは f32 で行ってから目的の型にする
            weight = try adapter.merged(try array(gguf, prefix + ".weight"), baseName: prefix + ".weight")
            if let dtype { weight = converted(weight, to: dtype) }
        } else {
            weight = try array(gguf, prefix + ".weight", dtype: dtype)
        }
        let linear = Linear(weight: weight, bias: try optionalArray(gguf, prefix + ".bias", dtype: dtype))
        let leaf = prefix.split(separator: ".").last.map(String.init) ?? prefix
        if let lora, lora.targets?.contains(leaf) == true {
            let wrapped = LoRALinear(base: linear, rank: lora.rank, alpha: lora.alpha)
            return (wrapped, wrapped)
        }
        return (linear, nil)
    }
}

/// llama.cpp 形式の LoRA アダプタ（`LoraAdapterWriter` が書く GGUF）をベースの重みに足し込む（推論用）。
///
/// llama.cpp の適用 `W·x + (alpha/rank)·Bᵀ(Aᵀx)` と同じ結果になるよう、読み込み時に
/// `W' = W + (alpha/rank)·B·A`（B: [out, rank]、A: [rank, in]。どちらも GGUF の行優先の形そのまま）にする。
/// アダプタのテンソルが 1 つでもベースに当たらなければ例外にする（黙ってベースで動くと
/// 「アダプタを使っているつもりで計測していた」事故になる。`ZenzEngine` と同じ方針）
public final class LoRAAdapterMerger {
    let gguf: GGUFFile
    let alpha: Float
    private(set) var mergedNames: Set<String> = []

    public init(path: String, baseArchitecture: String?) throws {
        gguf = try GGUFFile(path: path)
        guard gguf.string("general.type") == "adapter", gguf.string("adapter.type") == "lora" else {
            throw TrainableLMError.missingMetadata("adapter.type=lora（LoRA アダプタではありません: \(path)）")
        }
        guard gguf.architecture == baseArchitecture else {
            throw TrainableLMError.unsupportedArchitecture(
                "アダプタ \(gguf.architecture ?? "(不明)") とベース \(baseArchitecture ?? "(不明)") が違います")
        }
        guard let alpha = gguf.float("adapter.lora.alpha") else {
            throw TrainableLMError.missingMetadata("adapter.lora.alpha")
        }
        self.alpha = alpha
    }

    /// アダプタが対象にしているベースのテンソル名
    public var targetNames: Set<String> {
        Set(gguf.tensors.compactMap { $0.name.hasSuffix(".lora_a") ? String($0.name.dropLast(".lora_a".count)) : nil })
    }

    func merged(_ weight: MLXArray, baseName: String) throws -> MLXArray {
        guard gguf.tensor(named: baseName + ".lora_a") != nil else { return weight }
        let a = try GGUFWeights.array(gguf, baseName + ".lora_a")  // [rank, in]
        let b = try GGUFWeights.array(gguf, baseName + ".lora_b")  // [out, rank]
        guard a.dim(1) == weight.dim(1), b.dim(0) == weight.dim(0), a.dim(0) == b.dim(1) else {
            throw TrainableLMError.missingTensor("\(baseName).lora_a/lora_b の形がベースと合いません")
        }
        mergedNames.insert(baseName)
        return weight + (alpha / Float(a.dim(0))) * matmul(b, a)
    }

    /// 読み込みが終わった時点で、アダプタのテンソルが全部ベースに当たったかを確かめる
    public func verifyAllMerged() throws {
        let missing = targetNames.subtracting(mergedNames)
        guard missing.isEmpty else {
            throw TrainableLMError.missingTensor("アダプタのテンソルがベースにありません: \(missing.sorted().prefix(3).joined(separator: ", "))")
        }
    }
}
