import Foundation

/// 記号・顔文字・絵文字の候補（`SymbolCandidateTable`）を文節の候補ウィンドウに合流させる規則（2026-10-16）。
///
/// - 絵文字は上から 7 番目に入れる（Mozc の `EmojiRewriter` の `kDefaultInsertPos = 6` と同じ）。
///   先頭の動かさない候補（今の表示・変換ルール・打ち間違いの訂正）がそれより下まで来ていれば、その後ろに入れる。
///   エンジン（azooKey の辞書）が既に出している絵文字は、その位置から絵文字の並びへ移す
/// - 顔文字・記号は、エンジンの候補の後ろに 顔文字 → 記号 の順で足す。既に出ているものは元の位置のまま
/// - ライブ変換には使わない（呼び出し側が候補ウィンドウを開くときだけ使う）
public enum SymbolCandidateMerger {

    /// 絵文字を入れる位置（0 始まり）
    public static let emojiPosition = 6

    /// - parameters:
    ///   - candidates: 候補ウィンドウの候補（先頭 `protectedCount` 件は動かさない）
    ///   - symbols: 文節の読みに対する記号・顔文字・絵文字
    public static func merge(_ candidates: [String], with symbols: SymbolCandidates,
                             protectedCount: Int) -> [String] {
        guard !symbols.isEmpty else { return candidates }
        let protectedCount = min(max(protectedCount, 0), candidates.count)
        let head = Array(candidates.prefix(protectedCount))
        let emojiSet = Set(symbols.emoji)
        var tail = candidates.dropFirst(protectedCount).filter { !emojiSet.contains($0) }

        var seen = Set(head).union(tail)
        let emoji = symbols.emoji.filter { seen.insert($0).inserted }
        let insertAt = min(max(protectedCount, emojiPosition), head.count + tail.count) - head.count
        tail.insert(contentsOf: emoji, at: insertAt)
        tail.append(contentsOf: (symbols.emoticons + symbols.symbols).filter { seen.insert($0).inserted })
        return head + tail
    }

    /// 確定しても学習しない候補: 表に載っていて、この読みで出た候補すべて（エンジンが出していたものも含む）。
    /// ただし漢字・かなだけでできた候補は除く（Mozc の記号データには部首の「女」（おんなへん）のような
    /// 普通の文字も入っていて、それらは普通の語と同じく学習してよい）
    public static func unlearnableCandidates(in candidates: [String], symbols: SymbolCandidates) -> Set<String> {
        Set(symbols.all).intersection(candidates).filter { !isKanaOrKanji($0) }
    }

    /// 漢字・ひらがな・カタカナ（長音符・踊り字を含む）だけでできているか
    static func isKanaOrKanji(_ text: String) -> Bool {
        !text.isEmpty && text.unicodeScalars.allSatisfy { scalar in
            scalar.properties.isIdeographic
                || (0x3041...0x309F).contains(scalar.value)   // ひらがな
                || (0x30A0...0x30FF).contains(scalar.value)   // カタカナ・長音符
                || (0x31F0...0x31FF).contains(scalar.value)   // カタカナの小書き
                || (0x3005...0x3006).contains(scalar.value)   // 々 〆
        }
    }
}
