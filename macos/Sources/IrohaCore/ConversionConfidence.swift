import Foundation

/// 変換結果の1文字ぶんの自信度（モデルが生成したときの確からしさ）。
/// 文字が複数トークンにまたがる・1トークンが複数文字を含む場合は、その文字に関わる
/// トークンのうち最も低い値を採る
public struct CharacterConfidence: Sendable, Equatable {
    /// この文字を出したトークンの対数確率（nat。0が確信、負に大きいほど迷い）
    public let logProb: Float
    /// 読み制約を満たすトークンのうち1位と2位の対数確率の差。
    /// 同音異義語で迷っているほど小さい。競合するトークンが無ければ +inf
    public let margin: Float
    /// 読み制約を諦めて素の最尤トークンを選んだ（読みを追えなくなった）
    public let relaxed: Bool

    public init(logProb: Float, margin: Float, relaxed: Bool) {
        self.logProb = logProb
        self.margin = margin
        self.relaxed = relaxed
    }

    /// 2つの値のうち自信の低い方をまとめる（複数トークンが1文字に関わるとき）
    func merged(with other: CharacterConfidence) -> CharacterConfidence {
        CharacterConfidence(
            logProb: min(logProb, other.logProb),
            margin: min(margin, other.margin),
            relaxed: relaxed || other.relaxed)
    }
}

/// 自信度つきの変換結果。`confidences` は `text` の Character と同じ数・同じ順
public struct ScoredConversion: Sendable {
    public let text: String
    /// 系列全体（終端まで）の対数確率
    public let logProb: Float
    public let confidences: [CharacterConfidence]

    public init(text: String, logProb: Float, confidences: [CharacterConfidence]) {
        self.text = text
        self.logProb = logProb
        self.confidences = confidences
    }
}

/// トークン列（バイト断片）を出力文字列の文字ごとの自信度に対応づける。
/// バイト単位のトークンが多バイト文字の途中で切れていても、その文字が完成した時点で
/// 関わったトークン全てをまとめて割り当てる
struct ConfidenceAligner {
    private var bytes = Data()
    private var pending: CharacterConfidence?
    private var assignedCharacters = 0
    private(set) var confidences: [CharacterConfidence] = []

    /// 1トークンぶんのバイト列とその自信度を追加する
    mutating func append(_ piece: Data, confidence: CharacterConfidence) {
        bytes.append(piece)
        pending = pending.map { $0.merged(with: confidence) } ?? confidence
        guard let text = String(data: bytes, encoding: .utf8) else { return }
        let count = text.count
        guard count > assignedCharacters, let pending else { return }
        confidences.append(contentsOf: repeatElement(pending, count: count - assignedCharacters))
        assignedCharacters = count
        self.pending = nil
    }

    /// 最終的な出力文字列（不正バイトの除去・前後の空白除去を経たもの）に長さを揃えて返す。
    /// `untrimmed` は空白除去前の文字列
    func aligned(untrimmed: String, trimmed: String) -> [CharacterConfidence] {
        var result = confidences
        // 途中に不正バイトがあって割り当てが止まった場合は、残りを保留中の値（無ければ末尾の値）で埋める
        if result.count < untrimmed.count {
            let filler = pending ?? result.last ?? CharacterConfidence(logProb: 0, margin: .infinity, relaxed: false)
            result.append(contentsOf: repeatElement(filler, count: untrimmed.count - result.count))
        }
        let leading = untrimmed.prefix { $0.isWhitespace || $0.isNewline }.count
        return Array(result.dropFirst(leading).prefix(trimmed.count))
    }
}

extension CharacterConfidence {
    /// LLMの外側（ユーザ辞書・学習・変換ルール）で決まった文字の自信度。強調の対象にしない
    public static let trusted = CharacterConfidence(logProb: 0, margin: .infinity, relaxed: false)
}

extension ScoredConversion {
    /// 全文字を信頼済みとした結果（自信度を出せない経路・辞書で埋めた部分）
    public static func trusted(_ text: String) -> ScoredConversion {
        ScoredConversion(text: text, logProb: 0,
                         confidences: Array(repeating: .trusted, count: text.count))
    }

    /// 後ろに別の結果を連結する（対数確率は足す）
    public func appending(_ other: ScoredConversion) -> ScoredConversion {
        ScoredConversion(text: text + other.text, logProb: logProb + other.logProb,
                         confidences: confidences + other.confidences)
    }

    /// 先頭 `count` 文字ぶんに切り詰める（区切りの変換で末尾の文節を次へ回すとき）
    public func prefix(_ count: Int) -> ScoredConversion {
        ScoredConversion(text: String(text.prefix(count)), logProb: logProb,
                         confidences: Array(confidences.prefix(count)))
    }

    /// `margin` が閾値未満の文字の範囲（Character オフセット）。隣り合う文字はひとつにまとめる。
    /// 制約を緩めた文字も含める
    public func lowConfidenceRanges(marginBelow threshold: Float) -> [Range<Int>] {
        var ranges: [Range<Int>] = []
        for (offset, confidence) in confidences.enumerated()
        where confidence.margin < threshold || confidence.relaxed {
            if let last = ranges.last, last.upperBound == offset {
                ranges[ranges.count - 1] = last.lowerBound..<(offset + 1)
            } else {
                ranges.append(offset..<(offset + 1))
            }
        }
        return ranges
    }
}
