import Foundation

/// 候補の列に、モデルから見た確からしさ（系列の対数確率）を付ける
public protocol CandidateScorer: Sendable {
    /// - Returns: `candidates` と同じ順の対数確率（大きいほど確からしい）
    func score(candidates: [String], reading: String, context: String) async throws -> [Float]
}

/// 辞書ラティスで候補を作り、LLM（zenz）で並べ替える変換エンジン。
///
/// zenzの自由生成は読みの合わない語を出すことがあり、読み制約（`ReadingConstraint`）では
/// 漢字・英字の読みを検証できない。候補ウィンドウ（`candidateCount > 1`）では候補の生成を
/// 辞書ラティス（`LatticeConverter`）に任せ、「読みが辞書で保証された候補」とzenz自身の生成結果を
/// zenzの対数確率で並べる。azooKey/Zenzaiと同じ「辞書で読みを保証し、モデルで順位付け」の役割分担
///
/// 第一候補（`candidateCount == 1`、ライブ変換）は既定でzenzの生成に任せる。
/// ラティスのn-bestは長い文で正解を含まないことが多く、AJIMEE-Bench（200件）では
/// zenz生成 84.5% に対しラティス候補の再採点は 66.5% と大きく劣るため
/// （`usesLatticeForFirstCandidate` は計測・実験用）
///
/// ラティスが候補を返せない読み（辞書にない語だけの入力など）はzenzの生成にそのまま任せる
public struct LatticeRescoringEngine<Base: ConversionEngine & CandidateScorer>: ConversionEngine {

    public let base: Base
    public let lattice: LatticeConverter
    /// ラティスから取り出して採点する候補数の下限（要求候補数の方が多ければそちら）
    public let latticeCandidateCount: Int
    /// 第一候補もラティス候補の再採点で決める（既定false。上記のとおり精度が落ちる）
    public let usesLatticeForFirstCandidate: Bool

    public init(base: Base, lattice: LatticeConverter, latticeCandidateCount: Int = 10,
                usesLatticeForFirstCandidate: Bool = false) {
        self.base = base
        self.lattice = lattice
        self.latticeCandidateCount = latticeCandidateCount
        self.usesLatticeForFirstCandidate = usesLatticeForFirstCandidate
    }

    public func prewarm() async throws {
        try await base.prewarm()
        _ = await lattice.candidates(reading: "うぉーむあっぷ", count: 1)
    }

    public func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        if candidateCount <= 1, !usesLatticeForFirstCandidate {
            return try await base.convert(reading: reading, context: context, candidateCount: candidateCount)
        }
        let latticeCandidates = await lattice.candidates(
            reading: reading, count: max(latticeCandidateCount, candidateCount))
        guard !latticeCandidates.isEmpty else {
            return try await base.convert(reading: reading, context: context, candidateCount: candidateCount)
        }
        try Task.checkCancellation()

        // zenz自身の答え（ライブ変換で表示しているもの）も一緒に採点する。
        // 辞書にない語（固有名詞など）はここからしか候補に入らない
        var candidates = latticeCandidates
        if let generated = try await base.convert(reading: reading, context: context, candidateCount: 1).first,
           !generated.isEmpty, !candidates.contains(generated) {
            candidates.append(generated)
        }
        try Task.checkCancellation()

        let scores = try await base.score(candidates: candidates, reading: reading, context: context)
        guard scores.count == candidates.count else {
            throw ConversionError.inferenceFailed("採点結果の数が候補と合いません")
        }
        let ranked = zip(candidates, scores)
            .filter { $0.1 > -.infinity }
            .sorted { $0.1 > $1.1 }
            .map(\.0)
        return Array(ranked.prefix(max(candidateCount, 1)))
    }
}
