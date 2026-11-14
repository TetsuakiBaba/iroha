import Foundation
import IrohaCore
import IrohaDocuments
import IrohaMLX

/// かな漢字変換エンジンの組み立て（デコレータの重ね方）。IME 本体・`iroha-cli`・`iroha-bench` が同じものを使う。
///
/// 外側から 学習 → ユーザ辞書 → 長い読みの区切り → 異体字の補完 → 辞書ラティス + モデルの採点 → 自分の原稿の表記 → モデル
/// の順に重ねる。学習・ユーザ辞書・原稿の表記は変換のたびに閉包から読むので、空を渡せば素通しになる
/// （評価ではモデル単体の力を測るため空にする）。
/// IrohaCore は MLX と macOS 専用の部分（`SystemReadingTokenizer`）に依存させないので、別のモジュールに置く
public enum EngineAssembly {

    /// ニューラルネットの実体
    public enum Model {
        case llamaCpp(ZenzEngine)
        case mlx(MLXConversionEngine)
    }

    /// 辞書ラティスの使い方
    public enum LatticeUse: Sendable {
        /// 使わない（モデルだけ。`IROHA_LATTICE=off`）
        case off
        /// 候補ウィンドウの候補にだけ使い、第一候補はモデルの生成のまま（IME の既定）
        case candidates
        /// 第一候補も辞書ラティスの候補をモデルで採点し直して選ぶ（`IROHA_LATTICE=always`）
        case firstCandidate
    }

    /// 一番外側の学習のデコレータを返す（IME は `engineResult(forReading:shown:)` で学習を除いた結果を引く）
    public static func make(
        model: Model,
        lattice: LatticeConverter?,
        latticeUse: LatticeUse = .candidates,
        notation: @escaping @Sendable () -> NotationPreferenceConfiguration = { .disabled },
        userDictionary: @escaping @Sendable () -> UserDictionary = { .empty },
        learning: @escaping @Sendable () -> LearningDictionary = { .empty }
    ) -> LearningEngine {
        let core: any ConversionEngine
        switch model {
        case .llamaCpp(let zenz):
            core = withLattice(withNotation(zenz, notation), lattice: lattice, use: latticeUse)
        case .mlx(let engine):
            core = withLattice(withNotation(engine, notation), lattice: lattice, use: latticeUse)
        }
        return LearningEngine(
            base: UserDictionaryEngine(
                base: ChunkedConversionEngine(base: VariantKanjiEngine(base: core)),
                dictionary: userDictionary,
                lattice: lattice),
            dictionary: learning)
    }

    /// 自分の原稿の表記をモデルの採点に加える。設定が OFF・文書が無いときは素通し（採点を呼ばない）
    private static func withNotation<Base: ConversionEngine & CandidateScorer>(
        _ base: Base, _ configuration: @escaping @Sendable () -> NotationPreferenceConfiguration
    ) -> NotationPreferenceEngine<Base> {
        NotationPreferenceEngine(base: base, tokenizer: SystemReadingTokenizer(), configuration: configuration)
    }

    private static func withLattice<Base: ConversionEngine & CandidateScorer>(
        _ base: Base, lattice: LatticeConverter?, use: LatticeUse
    ) -> any ConversionEngine {
        guard use != .off, let lattice else { return base }
        return LatticeRescoringEngine(base: base, lattice: lattice, usesLatticeForFirstCandidate: use == .firstCandidate)
    }
}
