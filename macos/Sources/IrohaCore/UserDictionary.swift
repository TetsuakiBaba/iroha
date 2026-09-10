import Foundation

/// ユーザ辞書の1エントリ。読みはひらがなに正規化して保持する。
public struct UserDictionaryEntry: Codable, Sendable, Hashable, Identifiable {

    /// エントリの出所。macOSのユーザ辞書から取り込んだものは同期時に更新・削除の対象になる。
    /// irohaの設定で編集したエントリは`.manual`に変わり、以後の同期では触られない
    public enum Source: String, Codable, Sendable {
        case manual   // irohaの設定で追加・編集した
        case system   // macOSのユーザ辞書から取り込んだ
    }

    public var id: UUID
    public var reading: String
    public var word: String
    public var source: Source

    public init(id: UUID = UUID(), reading: String, word: String, source: Source = .manual) {
        self.id = id
        self.reading = UserDictionary.normalizedReading(reading)
        self.word = word
        self.source = source
    }
}

/// 変換時に参照する不変のスナップショット（読み→単語の索引つき）。
public struct UserDictionary: Sendable {

    /// 読みを辞書一致部分とそれ以外に分けた結果
    public enum Chunk: Equatable, Sendable {
        case word(String)      // ユーザ辞書の単語（変換済み）
        case reading(String)   // 変換エンジンに渡す読み
    }

    public static let empty = UserDictionary(entries: [])

    public let entries: [UserDictionaryEntry]
    /// 読み → 単語（登録順、重複なし）。候補ウィンドウ用の全エントリ
    private let wordsByReading: [String: [String]]
    /// 読み → 単語のうちライブ変換にも使うもの（`isSuitableForLiveConversion` を満たす）
    private let liveWordsByReading: [String: [String]]
    /// 読み → 単語のうち候補ウィンドウにだけ出すもの（学習の除外判定に使う）
    private let candidateOnlyWordsByReading: [String: [String]]
    /// 索引にある読みの最大文字数（最長一致の走査上限）
    private let maxReadingLength: Int

    public init(entries: [UserDictionaryEntry]) {
        self.entries = entries
        var index: [String: [String]] = [:]
        var liveIndex: [String: [String]] = [:]
        var candidateOnlyIndex: [String: [String]] = [:]
        var maxLength = 0
        for entry in entries {
            let reading = Self.normalizedReading(entry.reading)
            guard !reading.isEmpty, !entry.word.isEmpty else { continue }
            var words = index[reading] ?? []
            guard !words.contains(entry.word) else { continue }
            words.append(entry.word)
            index[reading] = words
            if Self.isSuitableForLiveConversion(reading: reading, word: entry.word) {
                liveIndex[reading, default: []].append(entry.word)
            } else {
                candidateOnlyIndex[reading, default: []].append(entry.word)
            }
            maxLength = max(maxLength, reading.count)
        }
        self.wordsByReading = index
        self.liveWordsByReading = liveIndex
        self.candidateOnlyWordsByReading = candidateOnlyIndex
        self.maxReadingLength = maxLength
    }

    public var isEmpty: Bool { wordsByReading.isEmpty }

    /// ライブ変換に使うエントリが1つも無いか（あれば素通しでよい）
    public var isEmptyForLiveConversion: Bool { liveWordsByReading.isEmpty }

    /// 読みに完全一致する単語（登録順）
    public func words(forReading reading: String) -> [String] {
        wordsByReading[Self.normalizedReading(reading)] ?? []
    }

    /// 読みに完全一致する単語のうちライブ変換に使うもの（登録順）
    public func liveWords(forReading reading: String) -> [String] {
        liveWordsByReading[Self.normalizedReading(reading)] ?? []
    }

    /// 読み（文節全体）の中に一致する、候補ウィンドウにだけ出す単語。
    ///
    /// これらは候補から選んで確定しても学習に記録しない。学習が覚えると、
    /// ライブ変換から除外した意味がなくなる（学習エンジンは辞書の外側にあるため）。
    /// 部分一致は`split`と同じく`minimumMatchLength`文字以上の読みだけを見る
    public func candidateOnlyWords(in reading: String, minimumMatchLength: Int = 2) -> [String] {
        guard !candidateOnlyWordsByReading.isEmpty else { return [] }
        let normalized = Self.normalizedReading(reading)
        guard !normalized.isEmpty else { return [] }
        var result: [String] = []
        for (key, words) in candidateOnlyWordsByReading
        where key == normalized || (key.count >= minimumMatchLength && normalized.contains(key)) {
            result.append(contentsOf: words)
        }
        return result
    }

    /// 単語をライブ変換の結果として埋め込んでよいか。
    ///
    /// 出力が読みより大幅に長いエントリ（「たぐ」→「#helloworld #dummytag」、
    /// 「めーる」→ メールアドレス、「おせわ」→ 定型文など）は文中に埋め込む語ではなく
    /// 入力の省略記法なので、ライブ変換では使わず候補ウィンドウにだけ出す（ことえりと同じ体感）。
    /// 漢字変換は読みより短くなるのが普通（「とうきょうと」→「東京都」）なので、
    /// 長くなる方向だけを見る。「かぶ」→「株式会社」（2倍・差2）程度の略記は通す
    public static func isSuitableForLiveConversion(reading: String, word: String) -> Bool {
        let readingLength = reading.count
        let wordLength = word.count
        let isMuchLonger = wordLength > readingLength * 2 && wordLength - readingLength >= 3
        return !isMuchLonger
    }

    /// 読み全体を、ユーザ辞書に一致する部分とそれ以外に左から最長一致で分割する。
    ///
    /// 1文字の読み（「あ」等）が文中で無差別に一致すると変換が壊れるため、
    /// 部分一致は`minimumMatchLength`文字以上のエントリだけを対象にする
    /// （完全一致は`words(forReading:)`が長さに関係なく拾う）。
    /// `forLiveConversion` が真ならライブ変換に使うエントリだけを一致の対象にする
    public func split(
        _ reading: String, minimumMatchLength: Int = 2, forLiveConversion: Bool = false
    ) -> [Chunk] {
        let table = forLiveConversion ? liveWordsByReading : wordsByReading
        guard !table.isEmpty, !reading.isEmpty else { return [.reading(reading)] }
        let characters = Array(reading)
        var chunks: [Chunk] = []
        var plain = ""
        var index = 0

        while index < characters.count {
            var match: (length: Int, word: String)?
            var length = min(maxReadingLength, characters.count - index)
            while length >= minimumMatchLength {
                let candidate = String(characters[index..<(index + length)])
                if let word = table[candidate]?.first {
                    match = (length, word)
                    break
                }
                length -= 1
            }
            if let match {
                if !plain.isEmpty {
                    chunks.append(.reading(plain))
                    plain = ""
                }
                chunks.append(.word(match.word))
                index += match.length
            } else {
                plain.append(characters[index])
                index += 1
            }
        }
        if !plain.isEmpty { chunks.append(.reading(plain)) }
        return chunks
    }

    // MARK: - 読みの正規化

    /// 読みをひらがなに正規化する（カタカナ入力・前後の空白を吸収）
    public static func normalizedReading(_ text: String) -> String {
        katakanaToHiragana(text.trimmingCharacters(in: .whitespacesAndNewlines))
    }

    /// irohaの読み（ローマ字入力の結果＝ひらがな）として成立する読みか。
    ///
    /// macOSのユーザ辞書には「omw」のようなASCIIショートカットも入っているが、
    /// irohaの読みはひらがなのため一致しようがない。取り込み時に除外する
    public static func isImportableReading(_ text: String) -> Bool {
        let reading = normalizedReading(text)
        guard !reading.isEmpty else { return false }
        return reading.unicodeScalars.allSatisfy { scalar in
            // ぁ〜ゖ・ゝゞ + 長音符
            (0x3041...0x3096).contains(scalar.value)
                || (0x309D...0x309E).contains(scalar.value)
                || scalar.value == 0x30FC
        }
    }
}
