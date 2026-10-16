import CoreText
import Foundation
import IrohaCore

/// 記号・顔文字・絵文字の候補（開発者モードの項目、既定ON・他のMacと同期する。2026-10-16）。
///
/// 文節の候補ウィンドウに Mozc の記号・顔文字・絵文字を足す（`SymbolCandidateTable` / `SymbolCandidateMerger`）。
/// 一般のユーザにはあるほうがよいので既定で ON にし、切り替えは開発者モードにだけ出す。
/// ライブ変換には使わず、選んで確定しても学習しない
enum SymbolCandidateSettings {

    static let enabledKey = "symbolCandidates"

    static var isEnabled: Bool {
        UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? true
    }

    /// 表（初めて候補ウィンドウを開いたときに読む。データが無ければ nil）
    static let table: SymbolCandidateTable? = {
        guard let directory = SymbolCandidateTable.defaultDirectoryURL() else { return nil }
        return SymbolCandidateTable(directory: directory, isRenderableEmoji: EmojiRenderability.isRenderable)
    }()
}

/// この Mac が絵文字を 1 つの絵として描けるか（Apple Color Emoji の字形で判定する）。
///
/// iroha は macOS 14 から動くが、Mozc のデータには新しい版の絵文字も入っていて、描けない OS では □ や
/// 部品の並びになる。OS の版と Emoji の版の対応表は持たず、実際の字形で見る:
/// すべての字形が Apple Color Emoji にあり（字形 0 ＝ 無し がない）、字形の数が部品（ZWJ・異体字セレクタを
/// 除いたコードポイント）の数より少なければ描ける。部品が 1 つなら字形も 1 つ。
/// 字形の数で 1 と決めないのは、組の絵文字（👨‍❤️‍👨）を Apple が左右 2 つの字形で描くため
enum EmojiRenderability {

    private static let font = CTFontCreateWithName("AppleColorEmoji" as CFString, 16, nil)

    static func isRenderable(_ emoji: String) -> Bool {
        let parts = emoji.unicodeScalars.filter { $0.value != 0x200D && !(0xFE00...0xFE0F).contains($0.value) }.count
        guard parts > 0 else { return false }
        let attributed = NSAttributedString(
            string: emoji, attributes: [NSAttributedString.Key(kCTFontAttributeName as String): font])
        let line = CTLineCreateWithAttributedString(attributed)
        guard let runs = CTLineGetGlyphRuns(line) as? [CTRun] else { return false }
        var glyphCount = 0
        for run in runs {
            let attributes = CTRunGetAttributes(run) as NSDictionary
            guard let value = attributes[kCTFontAttributeName as String] else { return false }
            let runFont = value as! CTFont
            guard CTFontCopyPostScriptName(runFont) as String == "AppleColorEmoji" else { return false }
            let count = CTRunGetGlyphCount(run)
            var glyphs = [CGGlyph](repeating: 0, count: count)
            CTRunGetGlyphs(run, CFRange(location: 0, length: count), &glyphs)
            guard !glyphs.contains(0) else { return false }
            glyphCount += count
        }
        return parts == 1 ? glyphCount == 1 : (1..<parts).contains(glyphCount)
    }
}
