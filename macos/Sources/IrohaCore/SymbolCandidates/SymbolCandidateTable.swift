import Foundation

/// 1 つの読みに対する記号・顔文字・絵文字の候補（`SymbolCandidateTable` が返す）
public struct SymbolCandidates: Sendable, Equatable {
    public var emoji: [String]
    public var emoticons: [String]
    public var symbols: [String]

    public init(emoji: [String] = [], emoticons: [String] = [], symbols: [String] = []) {
        self.emoji = emoji
        self.emoticons = emoticons
        self.symbols = symbols
    }

    public var isEmpty: Bool { emoji.isEmpty && emoticons.isEmpty && symbols.isEmpty }

    /// 絵文字 → 顔文字 → 記号 の順にすべて
    public var all: [String] { emoji + emoticons + symbols }
}

/// 読みから記号・顔文字・絵文字を引く表（2026-10-16）。
///
/// データは Mozc（BSD 3-Clause、Google）の `src/data/` から `macos/scripts/fetch-symbols.sh` が
/// コミット固定で取ってきた 4 ファイル（`symbol.tsv`・`emoticon.tsv`・`categorized.tsv`・`emoji_data.tsv`）。
/// azooKey の辞書にも記号・絵文字は入っているが、Mozc と比べると記号で 5%・顔文字で 1% ほどしかない
/// （2026-10-16 実測）。候補ウィンドウにだけ合流させ（`SymbolCandidateMerger`）、ライブ変換には使わない。
/// 読みは Mozc に合わせて「えもじ」で全部の絵文字、「かおもじ」で全部の顔文字を返す
public struct SymbolCandidateTable: Sendable {

    public static let allEmojiReading = "えもじ"
    public static let allEmoticonsReading = "かおもじ"

    /// バンドル・リポジトリ内で探すときのファイル名
    public static let fileNames = ["symbol.tsv", "emoticon.tsv", "categorized.tsv", "emoji_data.tsv"]

    private var emoji: [String: [String]] = [:]
    private var emoticons: [String: [String]] = [:]
    private var symbols: [String: [String]] = [:]
    private var allEmoji: [String] = []
    private var allEmoticons: [String] = []

    /// 各 TSV の中身から作る。
    /// - parameter isRenderableEmoji: OS が描ける絵文字か。描けないものは □ になるので候補に出さない
    ///   （判定は OS ごとの字形の有無によるので呼び出し側が渡す）
    public init(symbolTSV: String, emoticonTSV: String, categorizedTSV: String, emojiTSV: String,
                isRenderableEmoji: (String) -> Bool = { _ in true }) {
        // symbol.tsv: 品詞 / 文字 / 読み（空白区切り）/ 説明 / ...。1 行目は見出し
        for fields in Self.rows(symbolTSV) where fields.count >= 3 && fields[0] != "POS" {
            Self.add(fields[1], readings: fields[2], to: &symbols)
        }
        // emoticon.tsv: 顔文字 / 読み（空白区切り）/ 分類。1 行目は見出し（先頭の列が空）
        for fields in Self.rows(emoticonTSV) where fields.count >= 2 {
            Self.add(fields[0], readings: fields[1], to: &emoticons, all: &allEmoticons)
        }
        // categorized.tsv: 顔文字 / 分類 / 読み（空白区切り）
        for fields in Self.rows(categorizedTSV) where fields.count >= 3 {
            Self.add(fields[0], readings: fields[2], to: &emoticons, all: &allEmoticons)
        }
        // emoji_data.tsv: コードポイント / 絵文字 / 読み（空白区切り）/ ... / Emoji の版
        for fields in Self.rows(emojiTSV) where fields.count >= 3 && isRenderableEmoji(fields[1]) {
            Self.add(fields[1], readings: fields[2], to: &emoji, all: &allEmoji)
        }
        // 顔文字は 2 つのファイルの両方に載っているものがある
        var seen: Set<String> = []
        allEmoticons = allEmoticons.filter { seen.insert($0).inserted }
    }

    /// `directory` の 4 ファイルから作る。1 つでも読めなければ nil
    public init?(directory: URL, isRenderableEmoji: (String) -> Bool = { _ in true }) {
        var texts: [String] = []
        for name in Self.fileNames {
            guard let text = try? String(contentsOf: directory.appendingPathComponent(name), encoding: .utf8)
            else { return nil }
            texts.append(text)
        }
        self.init(symbolTSV: texts[0], emoticonTSV: texts[1], categorizedTSV: texts[2], emojiTSV: texts[3],
                  isRenderableEmoji: isRenderableEmoji)
    }

    /// データの置き場所: 環境変数 `IROHA_SYMBOLS` → .app の `Contents/Resources/Symbols` →
    /// カレントから 4 階層上までの `vendor/mozc-data`（iroha-cli・テスト用）
    public static func defaultDirectoryURL() -> URL? {
        var candidates: [URL] = []
        if let path = ProcessInfo.processInfo.environment["IROHA_SYMBOLS"], !path.isEmpty {
            candidates.append(URL(fileURLWithPath: path, isDirectory: true))
        }
        if let resources = Bundle.main.resourceURL {
            candidates.append(resources.appendingPathComponent("Symbols", isDirectory: true))
        }
        var directory = URL(fileURLWithPath: FileManager.default.currentDirectoryPath, isDirectory: true)
        for _ in 0..<4 {
            candidates.append(directory.appendingPathComponent("vendor/mozc-data", isDirectory: true))
            directory.deleteLastPathComponent()
        }
        return candidates.first { url in
            fileNames.allSatisfy { FileManager.default.fileExists(atPath: url.appendingPathComponent($0).path) }
        }
    }

    /// 読み（ひらがな。カタカナでもよい）に一致する候補。各種類の中はデータの並び順
    public func candidates(forReading reading: String) -> SymbolCandidates {
        let key = UserDictionary.normalizedReading(reading)
        return SymbolCandidates(
            emoji: key == Self.allEmojiReading ? allEmoji : emoji[key] ?? [],
            emoticons: key == Self.allEmoticonsReading ? allEmoticons : emoticons[key] ?? [],
            symbols: symbols[key] ?? [])
    }

    /// 読みの数・候補の数（`iroha-cli symbols` の表示用）
    public var counts: (emoji: Int, emoticons: Int, symbols: Int) {
        (allEmoji.count, allEmoticons.count, Set(symbols.values.joined()).count)
    }

    // MARK: - 読み込み

    private static func rows(_ text: String) -> [[String]] {
        text.split(whereSeparator: \.isNewline).compactMap { line in
            guard !line.hasPrefix("#") else { return nil }
            return line.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
        }
    }

    private static func add(_ value: String, readings: String, to table: inout [String: [String]]) {
        var unused: [String] = []
        add(value, readings: readings, to: &table, all: &unused)
    }

    /// 読みのうち、ひらがなだけのもの（irohaの読みとして打てるもの）に `value` を登録する。
    /// Mozc の読みには「,」「:)」のような記号そのものも入っているが、irohaの読みはひらがななので使わない
    private static func add(_ value: String, readings: String, to table: inout [String: [String]],
                            all: inout [String]) {
        guard !value.isEmpty else { return }
        var registered = false
        for reading in readings.split(separator: " ").map(String.init)
        where UserDictionary.isImportableReading(reading) {
            let key = UserDictionary.normalizedReading(reading)
            if !(table[key]?.contains(value) ?? false) {
                table[key, default: []].append(value)
            }
            registered = true
        }
        if registered {
            all.append(value)
        }
    }
}
