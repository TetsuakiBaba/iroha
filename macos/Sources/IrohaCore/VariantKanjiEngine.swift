import Foundation

/// 人名などで使われる異体字を候補ウィンドウの末尾に補う変換エンジン。
///
/// 髙（はしごだか）・﨑（たちざき）・德・濵 のような異体字は、辞書には「髙橋」「宮﨑」の
/// 複合語としてしか入っていないことが多く、「たか」「さき」と単独で打っても候補に出ない。
/// LLMもこれらをほぼ生成しない。そこで読みが完全一致するときだけ、小さな内蔵表から
/// 異体字を候補の末尾に足す（既にある候補は重複させない）。
/// ライブ変換（`candidateCount == 1`）には影響しない
public struct VariantKanjiEngine: ConversionEngine {

    public let base: any ConversionEngine

    public init(base: any ConversionEngine) {
        self.base = base
    }

    public func prewarm() async throws {
        try await base.prewarm()
    }

    public func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        let candidates = try await base.convert(reading: reading, context: context, candidateCount: candidateCount)
        guard candidateCount > 1 else { return candidates }
        let variants = Self.variants(forReading: reading).filter { !candidates.contains($0) }
        return candidates + variants
    }

    /// 読み（ひらがな）→ 異体字（`VariantKanjiTable` を参照）
    public static func variants(forReading reading: String) -> [String] {
        VariantKanjiTable.variants(forReading: reading)
    }
}

/// 単独では辞書に入りにくい人名用の異体字（旧字体・俗字）の表。
/// 通常の字体（高・崎・徳…）は辞書とLLMが出すので載せない
public enum VariantKanjiTable {

    /// 読み（ひらがな）→ 異体字。読みは `UserDictionary.normalizedReading` と同じ正規化で照合する
    public static func variants(forReading reading: String) -> [String] {
        table[UserDictionary.normalizedReading(reading)] ?? []
    }

    static let table: [String: [String]] = [
        "たか": ["髙"],
        "こう": ["髙"],
        "さき": ["﨑", "嵜"],
        "ざき": ["﨑", "嵜"],
        "とく": ["德"],
        "はま": ["濵", "濱"],
        "よし": ["𠮷"],
        "さわ": ["澤"],
        "ざわ": ["澤"],
        "たき": ["瀧"],
        "ひろ": ["廣"],
        "さい": ["齋", "齊"],
        "しぶ": ["澁"],
        "なべ": ["邊", "邉"],
        "べ": ["邊", "邉"],
        "くに": ["國"],
        "ま": ["眞"],
        "しん": ["眞"],
        "やなぎ": ["栁"],
        "かめ": ["龜"],
        "いわ": ["巖"],
        "しま": ["嶌"],
        "つち": ["圡"],
        "とみ": ["冨"],
        "くろ": ["黑"],
        "えい": ["榮"],
        "さくら": ["櫻"],
        "つる": ["鶴", "靍", "靏"],
        "はし": ["槗"],
        "みね": ["峯"],
        "ふじ": ["冨士"],
        "いち": ["壹"],
        "まん": ["萬"],
        "りゅう": ["龍", "竜"],
        "よう": ["樣"],
    ]
}
