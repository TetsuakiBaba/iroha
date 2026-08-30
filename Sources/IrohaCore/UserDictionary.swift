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
    /// 読み → 単語（登録順、重複なし）
    private let wordsByReading: [String: [String]]
    /// 索引にある読みの最大文字数（最長一致の走査上限）
    private let maxReadingLength: Int

    public init(entries: [UserDictionaryEntry]) {
        self.entries = entries
        var index: [String: [String]] = [:]
        var maxLength = 0
        for entry in entries {
            let reading = Self.normalizedReading(entry.reading)
            guard !reading.isEmpty, !entry.word.isEmpty else { continue }
            var words = index[reading] ?? []
            guard !words.contains(entry.word) else { continue }
            words.append(entry.word)
            index[reading] = words
            maxLength = max(maxLength, reading.count)
        }
        self.wordsByReading = index
        self.maxReadingLength = maxLength
    }

    public var isEmpty: Bool { wordsByReading.isEmpty }

    /// 読みに完全一致する単語（登録順）
    public func words(forReading reading: String) -> [String] {
        wordsByReading[Self.normalizedReading(reading)] ?? []
    }

    /// 読み全体を、ユーザ辞書に一致する部分とそれ以外に左から最長一致で分割する。
    ///
    /// 1文字の読み（「あ」等）が文中で無差別に一致すると変換が壊れるため、
    /// 部分一致は`minimumMatchLength`文字以上のエントリだけを対象にする
    /// （完全一致は`words(forReading:)`が長さに関係なく拾う）。
    public func split(_ reading: String, minimumMatchLength: Int = 2) -> [Chunk] {
        guard !isEmpty, !reading.isEmpty else { return [.reading(reading)] }
        let characters = Array(reading)
        var chunks: [Chunk] = []
        var plain = ""
        var index = 0

        while index < characters.count {
            var match: (length: Int, word: String)?
            var length = min(maxReadingLength, characters.count - index)
            while length >= minimumMatchLength {
                let candidate = String(characters[index..<(index + length)])
                if let word = wordsByReading[candidate]?.first {
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
