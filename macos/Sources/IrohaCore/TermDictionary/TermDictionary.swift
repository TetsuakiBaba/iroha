import Foundation
import KanaKanjiConverterModule

/// 専門辞書（設定 > 辞書・学習 > 専門辞書 から入れる「読み → 語」の一覧）を、辞書ラティスが読める形にしたもの。
///
/// 使い道は**候補ウィンドウだけ**: 辞書ラティス（`LatticeConverter`）が候補を作るときに語として加わり、
/// できた候補をモデルの対数確率で並べる。ライブ変換の第一候補（モデルの生成）は変えない。
/// ユーザ辞書のように読みが一致したら置き換える形にしないのは、語数の多い辞書では
/// 「かんせん → 感染」が文脈と関係なく「観戦」を上書きするなど、ほかの文の変換を壊すため。
/// 選んで確定した語は、今の学習の仕組みで次から第一候補になる。
///
/// azooKey のユーザ辞書の形式（LOUDS。`DictionaryBuilder.exportDictionary`）に書き出して、
/// ラティスにユーザ辞書として読ませる。動的ユーザ辞書（`importDynamicUserDictionary`）は使わない:
/// ラティスは読みの頭からの部分が本体の辞書のどの語の頭とも一致しなくなった所で先を探すのをやめ、
/// 動的ユーザ辞書の語はその判定に加わらないので、頭の部分が本体の辞書に無い語（多くの人名・地名）に届かない。
/// また動的ユーザ辞書は引くたびに全件をなめる。
///
/// 辞書のファイルは UTF-8 のテキストで、1 行が「読み<TAB>語」（読みはひらがな）。「#」で始まる行は注記で飛ばす
/// （`macos/scripts/build-term-dictionaries.py` が作る）
public struct TermDictionary: Sendable, Equatable {

    /// 語の種類。辞書ラティスでの品詞（前後のつながりやすさ）に使う
    public enum Kind: String, Codable, Sendable {
        case noun        // 用語（一般名詞）
        case person      // 人名
        case place       // 地名・駅名
        case properNoun  // そのほかの固有名詞（神社・寺院など）
    }

    public static let empty = TermDictionary(latticeDirectory: nil, entryCount: 0)

    /// azooKey のユーザ辞書の形式に書き出したフォルダ（`user.louds` などがある）。無ければ nil
    public let latticeDirectory: URL?
    /// 書き出した「読み・語」の組の数
    public let entryCount: Int

    public init(latticeDirectory: URL?, entryCount: Int) {
        self.latticeDirectory = latticeDirectory
        self.entryCount = entryCount
    }

    public var isEmpty: Bool { latticeDirectory == nil }

    /// 1文字の読みは文中のどこにでも当たるので入れない
    public static let minimumReadingLength = 2

    /// ラティスでの評価値。ユーザ辞書の語を語の境目の判定に入れるときと同じ -10
    /// （`LatticeConverter.wordBoundaries`。azooKey の評価用のコマンドと同じ値）。
    /// 候補ウィンドウでの順位はモデルの採点で決まるので、ここでは辞書の候補の上位（採点する範囲）に入れば足りる
    static let latticeValue: Float = -10

    // MARK: - ファイル

    /// 辞書ファイルの中身を「読み・語」の組にする。形の合わない行は飛ばす
    public static func parse(_ text: String) -> [(reading: String, word: String)] {
        var pairs: [(reading: String, word: String)] = []
        text.enumerateLines { line, _ in
            guard !line.hasPrefix("#"), let tab = line.firstIndex(of: "\t") else { return }
            let reading = String(line[..<tab])
            let word = String(line[line.index(after: tab)...])
            guard UserDictionary.isImportableReading(reading), !word.isEmpty, !word.contains("\t") else { return }
            pairs.append((reading, word))
        }
        return pairs
    }

    // MARK: - 書き出し

    /// 辞書ごとの「読み・語」の組を azooKey のユーザ辞書の形式で `directory` に書き出す。
    ///
    /// 同じ読みと語の組は最初の辞書のものだけを残す。`dictionaryURL` は本体の辞書のフォルダ
    /// （文字の番号表 `louds/charID.chid` を本体と合わせる必要がある）。
    /// - Returns: 書き出した組の数
    @discardableResult
    public static func export(
        packs: [(kind: Kind, pairs: [(reading: String, word: String)])],
        dictionaryURL: URL, to directory: URL
    ) throws -> Int {
        var seen: Set<String> = []
        var elements: [DicdataElement] = []
        for pack in packs {
            let cid = cid(for: pack.kind)
            for pair in pack.pairs {
                let reading = UserDictionary.normalizedReading(pair.reading)
                guard reading.count >= minimumReadingLength, !pair.word.isEmpty,
                      seen.insert(reading + "\t" + pair.word).inserted
                else { continue }
                elements.append(DicdataElement(
                    word: pair.word, ruby: hiraganaToKatakana(reading),
                    cid: cid, mid: MIDData.一般.mid, value: PValue(latticeValue)))
            }
        }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try DictionaryBuilder.exportDictionary(
            entries: elements, to: directory, baseName: "user", shardByFirstCharacter: false,
            charIDFileURL: dictionaryURL.appendingPathComponent("louds/charID.chid"))
        return elements.count
    }

    private static func cid(for kind: Kind) -> Int {
        switch kind {
        case .noun: CIDData.一般名詞.cid
        case .person: CIDData.人名一般.cid
        case .place: CIDData.地名一般.cid
        case .properNoun: CIDData.固有名詞.cid
        }
    }
}
