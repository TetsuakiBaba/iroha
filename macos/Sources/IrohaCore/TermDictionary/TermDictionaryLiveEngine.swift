import Foundation

/// 「ライブ変換にも使う」にした専門辞書の語を、ユーザ辞書と同じ規則で読みに当てる変換エンジンのデコレータ。
///
/// ユーザ辞書のデコレータ（`UserDictionaryEngine`）のすぐ内側に置く。ユーザ辞書に一致した部分はユーザ辞書の語になり、
/// 残りの読みがここに来るので、同じ読みならユーザ辞書の語が勝つ。
/// 当て方は `UserDictionaryEngine` そのもの（読み全体の一致は置き換え、途中の一致は辞書ラティスで語の境目に
/// なる所だけ。候補ウィンドウでは辞書の語を先頭に出す）。違うのは、使う語を読みごとに辞書ファイルから引くこと
/// （`TermDictionaryLookup.liveDictionary(occurringIn:)`。人名だけで 21 万語あり、メモリに展開しない）と、
/// 同じ読みに語が 2 つ以上ある読みは使わないこと。
///
/// ライブ変換に使う辞書が無ければ素通しする（既定のふるまいと同じ）
public final class TermDictionaryLiveEngine: ConversionEngine {

    private let base: any ConversionEngine
    private let lookup: @Sendable () -> TermDictionaryLookup
    private let lattice: LatticeConverter?

    public init(
        base: any ConversionEngine,
        lookup: @escaping @Sendable () -> TermDictionaryLookup,
        lattice: LatticeConverter? = nil
    ) {
        self.base = base
        self.lookup = lookup
        self.lattice = lattice
    }

    public func prewarm() async throws {
        try await base.prewarm()
    }

    public func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        let dictionary = lookup().liveDictionary(occurringIn: reading)
        guard !dictionary.isEmpty else {
            return try await base.convert(reading: reading, context: context, candidateCount: candidateCount)
        }
        return try await UserDictionaryEngine(base: base, dictionary: { dictionary }, lattice: lattice)
            .convert(reading: reading, context: context, candidateCount: candidateCount)
    }
}
