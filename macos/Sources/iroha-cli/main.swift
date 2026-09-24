import Foundation
import IrohaCore

// 変換エンジンの検証用CLIハーネス。
//
// 使い方:
//   iroha-cli kana <romaji>                       : ローマ字→かな変換のみ
//   iroha-cli convert [--context 文脈] <読み>      : かな漢字変換（読みはローマ字/ひらがなどちらでも）
//   iroha-cli segment <読み>                       : 変換 + 文節分割の検証
//   iroha-cli bench <eval.tsv>                    : 評価（TSV: 読み\t正解[\t左文脈]）。精度とレイテンシを報告
//   iroha-cli ajimee <evaluation_items.json>      : AJIMEE-Bench評価（acc@1・MinCER）。scripts/fetch-ajimee.shで取得
//   iroha-cli ajimee-dump <evaluation_items.json> <out.jsonl> [--lattice 件数]
//                                                 : AJIMEE各項目の辞書ラティス候補・zenz生成・zenz採点をJSONLに出す
//                                                   （experiments/jev/ の選択式判定実験の入力）
//   iroha-cli lattice-dump <train.txt|eval.tsv> <out.jsonl> [--n 件数] [--limit 件数] [--skip 件数]
//                                                 : 学習テキスト（U+EE02文脈 U+EE00読み U+EE01正解 の行、または
//                                                   TSV 読み\t正解[\t文脈]）の各行に辞書ラティスの読み一致候補を付けて
//                                                   JSONLに出す。zenzは読まない（experiments/reranker/ の学習データ）
//   iroha-cli confidence <evaluation_items.json> [--margin 閾値] [--examples 件数]
//                                                 : 自信度（文字ごとのマージン）と誤変換箇所の対応を
//                                                   AJIMEE-Benchで評価（zenz単体。誤変換検出の閾値設計用）
//   iroha-cli repl                                : 対話モード（1行ずつ変換、レイテンシ表示）
//   iroha-cli lattice <読み>                       : 辞書ラティス（azooKey）の生の候補を表示（調査用）
//   iroha-cli predict [--chain 回数] <左文脈>        : 予測（左文脈の続き1文節）。--chainで採用を繰り返す
//   iroha-cli typo catalog [install|remove]        : 配布中の訂正モデルの一覧・取得・削除（設定画面と同じ経路）
//   iroha-cli typo parity                         : 打ち間違い訂正モデルの移植検証（parity.json 200件）
//   iroha-cli typo bench <test.jsonl> [--n 件数]   : 同モデルのレイテンシ（mean / p50 / p95）
//   iroha-cli typo eval <test.jsonl> [--n 件数]    : 同モデルのθ別の訂正率・過剰訂正率（threshold_curve.py 相当）
//   iroha-cli typo prefix <test.jsonl> [--n 件数]  : 入力途中の読み（文字数で機械的に切る）に訂正を出す割合
//   iroha-cli typo pause <test.jsonl> [--n 件数]   : 文節の境目（人が入力を止めそうな場所）で切ったときの誤検出率
//   iroha-cli typo segments <test.jsonl> [--n 件数]: 採用した訂正が1文節に収まる割合（実際の変換・文節分割で測る）
//   iroha-cli typo shrink <出力先>                 : 重みを float16 に落として半分にする（落としたら parity を再実行）
//   iroha-cli typo <読み> [--threshold θ]          : 同モデルを1件試す（生成・margin・採否）
//   環境変数 IROHA_TYPO_MODEL で打ち間違い訂正モデルの置き場所を、IROHA_TYPO_CATALOG で
//   配布カタログのURLを上書きできる（既定は <データフォルダ>/models/typo-normalizer と GitHub）
//   環境変数 IROHA_MODEL でモデルパス、IROHA_LORA で追加学習した LoRA アダプタ（GGUF）、
//   IROHA_USER_DICT でユーザ辞書、IROHA_LEARNING で学習結果のファイルを上書き可能。
//   bench / ajimee はモデルの素の力を測るため、既定でユーザ辞書・学習を空にする
//   （IROHA_WITH_USER_DATA=1 でIME本体のデータを使う。IROHA_USER_DICT / IROHA_LEARNING の
//   明示指定はそのまま使う）。convert / segment / repl はIME本体と同じデータを使う
//   IROHA_LATTICE=off で辞書ラティスを使わずzenz単体、IROHA_LATTICE=always で第一候補も
//   ラティス候補の再採点で決める（既定は候補ウィンドウのみラティス。IME本体と同じ）
//   IROHA_NO_LATIN=1 で読みにラテン文字がないときの英字出力を禁じる（実験用。USB等も出なくなる）
//   IROHA_NO_CONSTRAINT=1 で読み制約（constrained decoding）を丸ごと切り素の貪欲生成にする
//   （実験用。Python側の制約なし計測と突き合わせるとき）
//   データフォルダはIME本体の設定（保存場所の変更）に従う。IROHA_DATA_DIR で上書き可能

/// IME本体と同じデータフォルダを使う（設定 > 情報 > データの保存場所 で変えた場所を追う）
func configureDataDirectory() {
    let env = ProcessInfo.processInfo.environment
    if let path = env["IROHA_DATA_DIR"], !path.isEmpty {
        DataDirectory.configure(URL(fileURLWithPath: path, isDirectory: true))
    } else if let path = UserDefaults(suiteName: "dev.iroha.inputmethod.iroha")?
        .string(forKey: "dataDirectory"), !path.isEmpty {
        DataDirectory.configure(URL(fileURLWithPath: path, isDirectory: true))
    }
}
configureDataDirectory()

func romajiToKana(_ input: String) -> String {
    // ASCII文字を含む場合のみローマ字として解釈する
    guard input.allSatisfy({ $0.isASCII }) else { return input }
    var composer = RomajiComposer()
    composer.input(input)
    composer.flush()
    return composer.display
}

/// レーベンシュタイン距離（CER算出用）
func editDistance(_ a: [Character], _ b: [Character]) -> Int {
    if a.isEmpty { return b.count }
    if b.isEmpty { return a.count }
    var previous = Array(0...b.count)
    var current = [Int](repeating: 0, count: b.count + 1)
    for i in 1...a.count {
        current[0] = i
        for j in 1...b.count {
            let substitution = previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)
            current[j] = min(previous[j] + 1, current[j - 1] + 1, substitution)
        }
        swap(&previous, &current)
    }
    return previous[b.count]
}


/// IROHA_MODEL / IROHA_LORA を反映した zenz エンジン。IME本体と同じ既定（モデル未指定なら既定モデル）
func makeZenz(restrictLatinToReading: Bool = false, usesReadingConstraint: Bool = true) -> ZenzEngine {
    let env = ProcessInfo.processInfo.environment
    let adapter = env["IROHA_LORA"].flatMap { $0.isEmpty ? nil : $0 }
    if let path = env["IROHA_MODEL"], !path.isEmpty {
        return ZenzEngine(modelPath: path, adapterPath: adapter, restrictLatinToReading: restrictLatinToReading,
                          usesReadingConstraint: usesReadingConstraint)
    }
    return ZenzEngine(adapterPath: adapter, restrictLatinToReading: restrictLatinToReading,
                      usesReadingConstraint: usesReadingConstraint)
}

/// ユーザ辞書・学習の扱い。評価（bench / ajimee）はモデル単体の力を測るので既定で空にする。
/// 実データを混ぜると学習ファイルの成長で同じモデルでも数値が変わり、再現性がなくなる
enum UserDataMode {
    /// IME本体と同じファイル（IROHA_USER_DICT / IROHA_LEARNING で差し替え可）
    case ime
    /// 空。IROHA_WITH_USER_DATA=1 なら `.ime` と同じ、IROHA_USER_DICT / IROHA_LEARNING の明示指定は使う
    case evaluation
}

/// IME本体と同じ構成（学習 + ユーザ辞書 + 長い読みの区切り + zenz）でエンジンを組み立てる。
/// IROHA_USER_DICT でユーザ辞書のJSONを差し替えられる（既定は本体と同じファイル）
func makeEngine(userData: UserDataMode = .ime) -> any ConversionEngine {
    let env = ProcessInfo.processInfo.environment
    let usesIMEData = userData == .ime || env["IROHA_WITH_USER_DATA"] == "1"
    // 存在しないファイルを指すストアは空として振る舞う（評価では書き込みも起きない）
    let emptyURL = FileManager.default.temporaryDirectory
        .appendingPathComponent("iroha-cli-empty-\(ProcessInfo.processInfo.processIdentifier)", isDirectory: true)
    let zenz: ZenzEngine
    // IROHA_NO_LATIN=1: 読みにラテン文字がなければ出力の英字を禁じる（英語語彙の多いモデルの評価用）
    let restrictLatin = ProcessInfo.processInfo.environment["IROHA_NO_LATIN"] == "1"
    // IROHA_NO_CONSTRAINT=1: 読み制約を切って素の貪欲生成にする（制約なし計測との突き合わせ用）
    let usesConstraint = ProcessInfo.processInfo.environment["IROHA_NO_CONSTRAINT"] != "1"
    zenz = makeZenz(restrictLatinToReading: restrictLatin, usesReadingConstraint: usesConstraint)
    if !usesConstraint {
        FileHandle.standardError.write("読み制約なし（IROHA_NO_CONSTRAINT=1）\n".data(using: .utf8)!)
    }
    let store: UserDictionaryStore
    if let path = env["IROHA_USER_DICT"] {
        store = UserDictionaryStore(url: URL(fileURLWithPath: path))
    } else if usesIMEData {
        store = .shared
    } else {
        store = UserDictionaryStore(url: emptyURL.appendingPathComponent("user-dictionary.json"))
    }
    let learning: LearningStore
    if let path = env["IROHA_LEARNING"] {
        learning = LearningStore(url: URL(fileURLWithPath: path))
    } else if usesIMEData {
        learning = .shared
    } else {
        learning = LearningStore(url: emptyURL.appendingPathComponent("learning.json"))
    }
    if userData == .evaluation {
        let note = usesIMEData ? "IME本体のユーザ辞書・学習を使用（IROHA_WITH_USER_DATA=1）"
            : "ユーザ辞書・学習は空（IME本体のデータを使うなら IROHA_WITH_USER_DATA=1）"
        FileHandle.standardError.write("\(note)\n".data(using: .utf8)!)
    }
    // 辞書ラティス + zenz採点（IME本体と同じ構成）。辞書が無い・OFF指定ならzenz単体
    let core: any ConversionEngine
    let latticeMode = ProcessInfo.processInfo.environment["IROHA_LATTICE"] ?? ""
    if latticeMode != "off", let dictionaryURL = LatticeConverter.defaultDictionaryURL() {
        core = LatticeRescoringEngine(
            base: zenz, lattice: LatticeConverter(dictionaryURL: dictionaryURL),
            usesLatticeForFirstCandidate: latticeMode == "always")
    } else {
        core = zenz
    }
    return LearningEngine(
        base: UserDictionaryEngine(
            base: ChunkedConversionEngine(base: VariantKanjiEngine(base: core)), dictionary: { store.current }),
        dictionary: { learning.current })
}

func convertAndPrint(engine: any ConversionEngine, reading: String, context: String, count: Int = 1) async {
    let kana = romajiToKana(reading)
    do {
        let start = ContinuousClock.now
        let candidates = try await engine.convert(reading: kana, context: context, candidateCount: count)
        let elapsed = start.duration(to: .now)
        let ms = Double(elapsed.components.attoseconds) / 1e15 + Double(elapsed.components.seconds) * 1e3
        var line = "\(kana) -> \(candidates.joined(separator: " / "))  [\(String(format: "%.1f", ms))ms]"
        // IMEと同じく、候補を並べる場面ではユーザ定義の変換ルールの出力も添える
        if count > 1 {
            let rewrites = UserRewriteRuleStore.shared.current.candidates(forReading: kana)
            if !rewrites.isEmpty { line += "  + ルール: \(rewrites.joined(separator: " / "))" }
        }
        print(line)
    } catch {
        FileHandle.standardError.write("エラー: \(error)\n".data(using: .utf8)!)
    }
}

/// ダウンロードの進捗を10%刻みで標準エラーに出す（コールバックは別スレッドから来る）
final class ProgressPrinter: @unchecked Sendable {
    static let shared = ProgressPrinter()
    private let lock = NSLock()
    private var lastBucket = -1

    func report(_ progress: Double) {
        let bucket = Int(progress * 10)
        lock.lock()
        let shouldPrint = bucket != lastBucket
        if shouldPrint { lastBucket = bucket }
        lock.unlock()
        guard shouldPrint else { return }
        FileHandle.standardError.write("  \(bucket * 10)%\n".data(using: .utf8)!)
    }
}

let arguments = CommandLine.arguments

switch arguments.count > 1 ? arguments[1] : "repl" {
case "kana" where arguments.count >= 3:
    print(romajiToKana(arguments[2]))

case "segment" where arguments.count >= 3:
    // 変換 + 文節分割の検証
    let kana = romajiToKana(arguments[2])
    let engine = makeEngine()
    do {
        let conversion = try await engine.convert(reading: kana, context: "", candidateCount: 1).first ?? kana
        let segments = ReadingAligner.segmentReading(kana, conversion: conversion)
        print("\(kana) -> \(segments.map(\.conversion).joined(separator: "|"))  (\(segments.map(\.reading).joined(separator: "|")))")
    } catch {
        FileHandle.standardError.write("エラー: \(error)\n".data(using: .utf8)!)
    }

case "bench" where arguments.count >= 3:
    // モデル評価: 完全一致率・文字誤り率(CER)・レイテンシを測る
    guard let content = try? String(contentsOfFile: arguments[2], encoding: .utf8) else {
        FileHandle.standardError.write("ファイルが読めません: \(arguments[2])\n".data(using: .utf8)!)
        exit(1)
    }
    // 3列目があれば左文脈（追加学習の held-out TSV。`TrainingDataBuilder.heldOutTSV`）
    let pairs: [(reading: String, expected: String, context: String)] = content
        .split(separator: "\n")
        .compactMap { line in
            let parts = line.split(separator: "\t", omittingEmptySubsequences: false)
            guard parts.count >= 2 else { return nil }
            return (String(parts[0]), String(parts[1]), parts.count >= 3 ? String(parts[2]) : "")
        }
    let engine = makeEngine(userData: .evaluation)
    // ウォームアップ（モデルロードを計測から除外）
    _ = try? await engine.convert(reading: "うぉーむあっぷ", context: "", candidateCount: 1)

    var exactMatches = 0
    var totalEditDistance = 0
    var totalExpectedLength = 0
    var totalMilliseconds = 0.0
    for (reading, expected, context) in pairs {
        let start = ContinuousClock.now
        let result = (try? await engine.convert(reading: reading, context: context, candidateCount: 1).first) ?? reading
        let elapsed = start.duration(to: .now)
        totalMilliseconds += Double(elapsed.components.attoseconds) / 1e15
            + Double(elapsed.components.seconds) * 1e3
        let distance = editDistance(Array(result), Array(expected))
        totalEditDistance += distance
        totalExpectedLength += expected.count
        if result == expected {
            exactMatches += 1
        } else {
            print("  ✗ \(reading) -> \(result) （正解: \(expected)）")
        }
    }
    let accuracy = Double(exactMatches) / Double(pairs.count) * 100
    let cer = Double(totalEditDistance) / Double(totalExpectedLength) * 100
    print(String(format: "件数: %d  完全一致: %d (%.1f%%)  CER: %.2f%%  平均: %.1fms/変換",
                 pairs.count, exactMatches, accuracy, cer, totalMilliseconds / Double(pairs.count)))

case "ajimee" where arguments.count >= 3:
    // AJIMEE-Bench (azooKey/AJIMEE-Bench) 評価。
    // zenzaiと同じ方式: グリーディ変換1候補を許容解リストと照合し、
    // acc@1（許容解のいずれかに完全一致）と MinCER（許容解との最小CERの平均）を報告する。
    // MinCERの定義は同リポジトリ utils.py に準拠（CER = 編集距離 / 正解長、項目ごとに最小値をとり平均）。
    struct AjimeeItem: Decodable {
        let index: String
        let contextText: String
        let input: String
        let expectedOutput: [String]
        enum CodingKeys: String, CodingKey {
            case index
            case contextText = "context_text"
            case input
            case expectedOutput = "expected_output"
        }
    }
    guard let data = FileManager.default.contents(atPath: arguments[2]),
          let items = try? JSONDecoder().decode([AjimeeItem].self, from: data) else {
        FileHandle.standardError.write("JSONが読めません: \(arguments[2])（scripts/fetch-ajimee.sh で取得できます）\n".data(using: .utf8)!)
        exit(1)
    }
    let engine = makeEngine(userData: .evaluation)
    _ = try? await engine.convert(reading: "うぉーむあっぷ", context: "", candidateCount: 1)

    struct Tally {
        var count = 0
        var accAt1 = 0
        var minCERSum = 0.0
        mutating func add(hit: Bool, minCER: Double) {
            count += 1
            if hit { accAt1 += 1 }
            minCERSum += minCER
        }
        var summary: String {
            String(format: "acc@1 %d/%d (%.1f%%)  MinCER %.2f%%",
                   accAt1, count, Double(accAt1) / Double(count) * 100,
                   minCERSum / Double(count) * 100)
        }
    }
    var withContext = Tally()
    var withoutContext = Tally()
    var totalMilliseconds = 0.0
    for item in items {
        let start = ContinuousClock.now
        let result = (try? await engine.convert(
            reading: item.input, context: item.contextText, candidateCount: 1
        ).first) ?? item.input
        let elapsed = start.duration(to: .now)
        totalMilliseconds += Double(elapsed.components.attoseconds) / 1e15
            + Double(elapsed.components.seconds) * 1e3
        let hit = item.expectedOutput.contains(result)
        let minCER = item.expectedOutput.map { reference in
            Double(editDistance(Array(result), Array(reference))) / Double(max(reference.count, 1))
        }.min() ?? 1.0
        if item.contextText.isEmpty {
            withoutContext.add(hit: hit, minCER: minCER)
        } else {
            withContext.add(hit: hit, minCER: minCER)
        }
        if !hit {
            print("  ✗ [\(item.index)] \(item.input) -> \(result) （正解: \(item.expectedOutput.joined(separator: " / "))）")
        }
    }
    if withoutContext.count > 0 { print("文脈なし: \(withoutContext.summary)") }
    if withContext.count > 0 { print("文脈あり: \(withContext.summary)") }
    var total = Tally()
    total.count = withContext.count + withoutContext.count
    total.accAt1 = withContext.accAt1 + withoutContext.accAt1
    total.minCERSum = withContext.minCERSum + withoutContext.minCERSum
    print(String(format: "全体: %@  平均 %.1fms/変換", total.summary, totalMilliseconds / Double(total.count)))

case "ajimee-dump" where arguments.count >= 4:
    // jev方式（選択肢ラベルのロジット直読み）実験用のダンプ。AJIMEE-Bench の各項目について
    // 辞書ラティスの読み一致候補（評価順）・zenz生成・zenzの対数確率を JSONL に書き出す。
    // Python 側（experiments/jev/）がこれを読んで別のLLMに「どれが正しいか」を選ばせる。
    //   iroha-cli ajimee-dump <evaluation_items.json> <out.jsonl> [--lattice 件数]
    struct AjimeeItem: Decodable {
        let index: String
        let contextText: String
        let input: String
        let expectedOutput: [String]
        enum CodingKeys: String, CodingKey {
            case index
            case contextText = "context_text"
            case input
            case expectedOutput = "expected_output"
        }
    }
    struct DumpRecord: Encodable {
        let index: String
        let context: String
        let reading: String
        let expected: [String]
        /// 辞書ラティスの読み一致候補（評価順、重複なし）
        let lattice: [String]
        /// zenz の貪欲生成（ライブ変換の第一候補）
        let generated: String
        /// `lattice` の先頭 `scored` 件 + generated（重複なら除く）に対する zenz の対数確率
        let scoredCandidates: [String]
        let zenzScores: [Float]
        let latticeMs: Double
        let generateMs: Double
        let scoreMs: Double
    }
    guard let data = FileManager.default.contents(atPath: arguments[2]),
          let items = try? JSONDecoder().decode([AjimeeItem].self, from: data) else {
        FileHandle.standardError.write("JSONが読めません: \(arguments[2])\n".data(using: .utf8)!)
        exit(1)
    }
    var latticeCount = 10
    if let i = arguments.firstIndex(of: "--lattice"), i + 1 < arguments.count { latticeCount = Int(arguments[i + 1]) ?? 10 }
    guard let dictionaryURL = LatticeConverter.defaultDictionaryURL() else {
        FileHandle.standardError.write("辞書が見つかりません（scripts/fetch-dictionary.sh）\n".data(using: .utf8)!)
        exit(1)
    }
    let lattice = LatticeConverter(dictionaryURL: dictionaryURL)
    let zenz = makeZenz()
    try await zenz.prewarm()
    _ = await lattice.candidates(reading: "うぉーむあっぷ", count: 1)
    func ms(_ start: ContinuousClock.Instant) -> Double {
        let d = start.duration(to: .now)
        return Double(d.components.seconds) * 1e3 + Double(d.components.attoseconds) / 1e15
    }
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.withoutEscapingSlashes]
    var lines: [String] = []
    for item in items {
        let t0 = ContinuousClock.now
        let latticeCandidates = await lattice.fullMatchCandidates(reading: item.input, nBest: latticeCount)
        let latticeMs = ms(t0)
        let t1 = ContinuousClock.now
        let generated = (try? await zenz.convert(reading: item.input, context: item.contextText, candidateCount: 1).first) ?? ""
        let generateMs = ms(t1)
        var toScore = Array(latticeCandidates.prefix(latticeCount))
        if !generated.isEmpty, !toScore.contains(generated) { toScore.append(generated) }
        let t2 = ContinuousClock.now
        let scores = (try? await zenz.score(candidates: toScore, reading: item.input, context: item.contextText)) ?? []
        let scoreMs = ms(t2)
        let record = DumpRecord(
            index: item.index, context: item.contextText, reading: item.input, expected: item.expectedOutput,
            lattice: latticeCandidates, generated: generated, scoredCandidates: toScore, zenzScores: scores,
            latticeMs: latticeMs, generateMs: generateMs, scoreMs: scoreMs)
        if let json = try? encoder.encode(record), let line = String(data: json, encoding: .utf8) {
            lines.append(line)
        }
        FileHandle.standardError.write(".".data(using: .utf8)!)
    }
    try (lines.joined(separator: "\n") + "\n").write(toFile: arguments[3], atomically: true, encoding: .utf8)
    FileHandle.standardError.write("\n\(lines.count) 件を \(arguments[3]) に書き出しました\n".data(using: .utf8)!)

case "lattice-dump" where arguments.count >= 4:
    // 候補選択専用モデル（experiments/reranker/）の学習データ作り。
    // 学習テキストの各行（読み・正解・任意の左文脈）に、辞書ラティスの読み一致候補（hard negative の源）を
    // 付けて JSONL に書く。zenz は使わない（ラティスだけなので 1 行 10ms 前後）。
    //   iroha-cli lattice-dump <train.txt|eval.tsv> <out.jsonl> [--n 10] [--limit N] [--skip N]
    // 入力の行形式は 2 種類を自動判別する:
    //   1. prepare_data.py の出力: [U+EE02 文脈]U+EE00 読み U+EE01 正解
    //   2. TSV: 読み\t正解[\t文脈]
    // 出力の 1 行: {"context","reading","gold","lattice":[読み一致候補（ラティス順・全件）],"goldRank":正解の位置(-1=なし),"ms"}
    struct LatticeDumpRecord: Encodable {
        let context: String
        let reading: String
        let gold: String
        let lattice: [String]
        let goldRank: Int
        let ms: Double
    }
    var nBest = 10
    var limit = Int.max
    var skip = 0
    var index = 4
    while index < arguments.count {
        switch arguments[index] {
        case "--n" where index + 1 < arguments.count:
            nBest = Int(arguments[index + 1]) ?? 10
            index += 2
        case "--limit" where index + 1 < arguments.count:
            limit = Int(arguments[index + 1]) ?? Int.max
            index += 2
        case "--skip" where index + 1 < arguments.count:
            skip = Int(arguments[index + 1]) ?? 0
            index += 2
        default:
            FileHandle.standardError.write("不明な引数: \(arguments[index])\n".data(using: .utf8)!)
            exit(1)
        }
    }
    guard let dictionaryURL = LatticeConverter.defaultDictionaryURL() else {
        FileHandle.standardError.write("辞書が見つかりません（scripts/fetch-dictionary.sh）\n".data(using: .utf8)!)
        exit(1)
    }
    guard let input = FileHandle(forReadingAtPath: arguments[2]) else {
        FileHandle.standardError.write("入力が読めません: \(arguments[2])\n".data(using: .utf8)!)
        exit(1)
    }
    let outputPath = arguments[3]
    guard FileManager.default.createFile(atPath: outputPath, contents: nil),
          let output = FileHandle(forWritingAtPath: outputPath) else {
        FileHandle.standardError.write("出力が開けません: \(outputPath)\n".data(using: .utf8)!)
        exit(1)
    }
    defer { try? output.close() }
    let lattice = LatticeConverter(dictionaryURL: dictionaryURL)
    _ = await lattice.candidates(reading: "うぉーむあっぷ", count: 1)
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.withoutEscapingSlashes]

    /// 1 行を (文脈, 読み, 正解) に分ける。形式が合わなければ nil
    func parseLine(_ line: String) -> (String, String, String)? {
        if line.contains("\u{EE00}") {
            guard let readingStart = line.range(of: "\u{EE00}"),
                  let outputStart = line.range(of: "\u{EE01}", range: readingStart.upperBound..<line.endIndex) else { return nil }
            var context = String(line[line.startIndex..<readingStart.lowerBound])
            if context.hasPrefix("\u{EE02}") { context.removeFirst() }
            let reading = String(line[readingStart.upperBound..<outputStart.lowerBound])
            let gold = String(line[outputStart.upperBound...])
            return (context, reading, gold)
        }
        let columns = line.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
        guard columns.count >= 2, !columns[0].isEmpty else { return nil }
        return (columns.count >= 3 ? columns[2] : "", columns[0], columns[1])
    }

    var lineNumber = 0
    var written = 0
    var skippedMalformed = 0
    var totalMs = 0.0
    var goldInLattice = 0
    var goldFirst = 0
    let started = ContinuousClock.now
    // 大きなファイルでも全体を読まずに済むよう、行ごとに読む
    for try await line in input.bytes.lines {
        lineNumber += 1
        if lineNumber <= skip { continue }
        if written >= limit { break }
        guard let (context, reading, gold) = parseLine(line), !reading.isEmpty, !gold.isEmpty else {
            skippedMalformed += 1
            continue
        }
        let t0 = ContinuousClock.now
        let candidates = await lattice.fullMatchCandidates(reading: reading, nBest: nBest)
        let d = t0.duration(to: .now)
        let ms = Double(d.components.seconds) * 1e3 + Double(d.components.attoseconds) / 1e15
        totalMs += ms
        let goldRank = candidates.firstIndex(of: gold) ?? -1
        if goldRank >= 0 { goldInLattice += 1 }
        if goldRank == 0 { goldFirst += 1 }
        let record = LatticeDumpRecord(context: context, reading: reading, gold: gold, lattice: candidates,
                                       goldRank: goldRank, ms: ms)
        guard var data = try? encoder.encode(record) else { continue }
        data.append(0x0A)
        output.write(data)
        written += 1
        if written % 1000 == 0 {
            let elapsed = started.duration(to: .now).components.seconds
            FileHandle.standardError.write(
                "\(written) 件  \(elapsed)s  ラティス平均 \(String(format: "%.1f", totalMs / Double(written)))ms\n".data(using: .utf8)!)
        }
    }
    let summary = String(
        format: "%d 件を %@ に書き出しました（形式不正 %d 行）。ラティス平均 %.1fms、正解がラティス内 %d (%.1f%%)、ラティス1位が正解 %d (%.1f%%)\n",
        written, outputPath, skippedMalformed, totalMs / Double(max(written, 1)),
        goldInLattice, Double(goldInLattice) / Double(max(written, 1)) * 100,
        goldFirst, Double(goldFirst) / Double(max(written, 1)) * 100)
    FileHandle.standardError.write(summary.data(using: .utf8)!)

case "confidence" where arguments.count >= 3:
    // 自信度の評価: 貪欲変換の各文字に付くマージン（読み制約を満たす1位と2位の対数確率差）が
    // 実際の誤変換箇所をどれだけ言い当てるかを AJIMEE-Bench で測る。
    // 文レベル（誤変換を含む文を光らせられるか）と文字レベル（光った文字が誤りか）の両方を出す。
    // 誤変換箇所は出力と最も近い許容解との編集距離の経路から求める（置換・挿入された出力文字。
    // 欠落は直後の出力文字に印を付ける）。
    struct AjimeeItem: Decodable {
        let index: String
        let contextText: String
        let input: String
        let expectedOutput: [String]
        enum CodingKeys: String, CodingKey {
            case index
            case contextText = "context_text"
            case input
            case expectedOutput = "expected_output"
        }
    }
    guard let data = FileManager.default.contents(atPath: arguments[2]),
          let items = try? JSONDecoder().decode([AjimeeItem].self, from: data) else {
        FileHandle.standardError.write("JSONが読めません: \(arguments[2])（scripts/fetch-ajimee.sh で取得できます）\n".data(using: .utf8)!)
        exit(1)
    }
    var exampleThreshold: Float = 1.0
    var exampleCount = 12
    var optionIndex = 3
    while optionIndex + 1 < arguments.count {
        switch arguments[optionIndex] {
        case "--margin": exampleThreshold = Float(arguments[optionIndex + 1]) ?? exampleThreshold
        case "--examples": exampleCount = Int(arguments[optionIndex + 1]) ?? exampleCount
        default: break
        }
        optionIndex += 2
    }

    let zenz = makeZenz()
    _ = try? await zenz.convertWithConfidence(reading: "うぉーむあっぷ", context: "")

    /// 出力の各文字が誤りか（最も近い許容解との編集距離の経路で判定）
    func errorMask(output: [Character], references: [String]) -> [Bool] {
        var best: (distance: Int, mask: [Bool])?
        for reference in references {
            let ref = Array(reference)
            let n = output.count, m = ref.count
            var table = [[Int]](repeating: [Int](repeating: 0, count: m + 1), count: n + 1)
            for i in 0...n { table[i][0] = i }
            for j in 0...m { table[0][j] = j }
            for i in stride(from: 1, through: n, by: 1) where n > 0 {
                for j in stride(from: 1, through: m, by: 1) where m > 0 {
                    let substitution = table[i - 1][j - 1] + (output[i - 1] == ref[j - 1] ? 0 : 1)
                    table[i][j] = min(table[i - 1][j] + 1, table[i][j - 1] + 1, substitution)
                }
            }
            let distance = table[n][m]
            if let best, distance >= best.distance { continue }
            // 経路を逆にたどる
            var mask = [Bool](repeating: false, count: n)
            var i = n, j = m
            while i > 0 || j > 0 {
                if i > 0, j > 0, table[i][j] == table[i - 1][j - 1] + (output[i - 1] == ref[j - 1] ? 0 : 1) {
                    if output[i - 1] != ref[j - 1] { mask[i - 1] = true }
                    i -= 1; j -= 1
                } else if i > 0, table[i][j] == table[i - 1][j] + 1 {
                    mask[i - 1] = true  // 余計な出力文字
                    i -= 1
                } else {
                    // 欠落: 直後の出力文字（末尾なら直前）に印
                    if i < n { mask[i] = true } else if n > 0 { mask[n - 1] = true }
                    j -= 1
                }
            }
            best = (distance, mask)
        }
        return best?.mask ?? [Bool](repeating: true, count: output.count)
    }

    /// AUROC（順位統計。同値は0.5）: score が大きいほど陽性らしいとみなす
    func auroc(positives: [Float], negatives: [Float]) -> Double {
        guard !positives.isEmpty, !negatives.isEmpty else { return .nan }
        var sum = 0.0
        for p in positives {
            for n in negatives {
                if p > n { sum += 1 } else if p == n { sum += 0.5 }
            }
        }
        return sum / Double(positives.count * negatives.count)
    }

    struct CharRecord { let isError: Bool; let margin: Float; let logProb: Float; let relaxed: Bool }
    struct ItemRecord {
        let index: String; let reading: String; let output: String; let references: [String]
        let hit: Bool; let mask: [Bool]; let confidences: [CharacterConfidence]
        var minMargin: Float { confidences.map(\.margin).min() ?? .infinity }
        var minLogProb: Float { confidences.map(\.logProb).min() ?? 0 }
        var relaxed: Bool { confidences.contains { $0.relaxed } }
    }
    var records: [ItemRecord] = []
    var chars: [CharRecord] = []
    var totalMilliseconds = 0.0
    var misaligned = 0
    for item in items {
        let start = ContinuousClock.now
        guard let result = try? await zenz.convertWithConfidence(reading: item.input, context: item.contextText) else { continue }
        let elapsed = start.duration(to: .now)
        totalMilliseconds += Double(elapsed.components.attoseconds) / 1e15 + Double(elapsed.components.seconds) * 1e3
        let output = Array(result.text)
        if result.confidences.count != output.count { misaligned += 1 }
        let mask = errorMask(output: output, references: item.expectedOutput)
        let hit = item.expectedOutput.contains(result.text)
        records.append(ItemRecord(index: item.index, reading: item.input, output: result.text,
                                  references: item.expectedOutput, hit: hit, mask: mask,
                                  confidences: result.confidences))
        for (offset, confidence) in result.confidences.enumerated() where offset < mask.count {
            chars.append(CharRecord(isError: mask[offset], margin: confidence.margin,
                                    logProb: confidence.logProb, relaxed: confidence.relaxed))
        }
    }

    let errorItems = records.filter { !$0.hit }
    let correctItems = records.filter(\.hit)
    print("件数: \(records.count)  acc@1: \(correctItems.count)  誤変換: \(errorItems.count)  " +
          String(format: "平均 %.1fms/変換", totalMilliseconds / Double(max(records.count, 1))) +
          (misaligned > 0 ? "  ⚠ 文字数不一致 \(misaligned)件" : ""))
    let errorChars = chars.filter(\.isError), correctChars = chars.filter { !$0.isError }
    print("文字数: \(chars.count)  誤り文字: \(errorChars.count)")
    print("読み制約を緩めた文: \(records.filter(\.relaxed).count)件（うち誤変換 \(errorItems.filter(\.relaxed).count)件）")
    print()
    print(String(format: "AUROC（文レベル: 最小マージンで誤変換文を見分ける）: %.3f",
                 auroc(positives: errorItems.map { -$0.minMargin }, negatives: correctItems.map { -$0.minMargin })))
    print(String(format: "AUROC（文レベル: 最小対数確率）: %.3f",
                 auroc(positives: errorItems.map { -$0.minLogProb }, negatives: correctItems.map { -$0.minLogProb })))
    print(String(format: "AUROC（文字レベル: マージンで誤り文字を見分ける）: %.3f",
                 auroc(positives: errorChars.map { -$0.margin }, negatives: correctChars.map { -$0.margin })))
    print(String(format: "AUROC（文字レベル: 対数確率）: %.3f",
                 auroc(positives: errorChars.map { -$0.logProb }, negatives: correctChars.map { -$0.logProb })))
    print()

    func percent(_ numerator: Int, _ denominator: Int) -> String {
        denominator == 0 ? "-" : String(format: "%.0f%%", Double(numerator) / Double(denominator) * 100)
    }
    print("## 閾値ごとの検出性能（マージン < 閾値 の文字を光らせる）")
    print()
    print("| 閾値 | 誤変換文の検出率 | 正しい文の誤警報率 | 光った文のうち誤変換 | 誤り文字の検出率 | 正しい文字の誤警報率 | 光った文字のうち誤り | 光る文字の割合 |")
    print("|---|---|---|---|---|---|---|---|")
    for threshold: Float in [0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0] {
        let flaggedError = errorItems.filter { $0.minMargin < threshold }.count
        let flaggedCorrect = correctItems.filter { $0.minMargin < threshold }.count
        let flaggedErrorChars = errorChars.filter { $0.margin < threshold }.count
        let flaggedCorrectChars = correctChars.filter { $0.margin < threshold }.count
        print("| \(threshold) | \(percent(flaggedError, errorItems.count)) | \(percent(flaggedCorrect, correctItems.count)) | " +
              "\(percent(flaggedError, flaggedError + flaggedCorrect)) | \(percent(flaggedErrorChars, errorChars.count)) | " +
              "\(percent(flaggedCorrectChars, correctChars.count)) | \(percent(flaggedErrorChars, flaggedErrorChars + flaggedCorrectChars)) | " +
              "\(percent(flaggedErrorChars + flaggedCorrectChars, chars.count)) |")
    }
    print()
    print("## 閾値ごとの検出性能（対数確率 < 閾値 の文字を光らせる）")
    print()
    print("| 閾値 | 誤変換文の検出率 | 正しい文の誤警報率 | 光った文のうち誤変換 | 誤り文字の検出率 | 正しい文字の誤警報率 | 光った文字のうち誤り | 光る文字の割合 |")
    print("|---|---|---|---|---|---|---|---|")
    for threshold: Float in [-0.25, -0.5, -0.75, -1.0, -1.5, -2.0, -3.0, -4.0] {
        let flaggedError = errorItems.filter { $0.minLogProb < threshold }.count
        let flaggedCorrect = correctItems.filter { $0.minLogProb < threshold }.count
        let flaggedErrorChars = errorChars.filter { $0.logProb < threshold }.count
        let flaggedCorrectChars = correctChars.filter { $0.logProb < threshold }.count
        print("| \(threshold) | \(percent(flaggedError, errorItems.count)) | \(percent(flaggedCorrect, correctItems.count)) | " +
              "\(percent(flaggedError, flaggedError + flaggedCorrect)) | \(percent(flaggedErrorChars, errorChars.count)) | " +
              "\(percent(flaggedCorrectChars, correctChars.count)) | \(percent(flaggedErrorChars, flaggedErrorChars + flaggedCorrectChars)) | " +
              "\(percent(flaggedErrorChars + flaggedCorrectChars, chars.count)) |")
    }
    print()

    // 誤変換文のうち、光った箇所が実際の誤り箇所に重なる割合（局所化の精度）
    print("## 局所化（誤変換文で、光った文字のいずれかが誤り文字の±1文字以内にあるか）")
    print()
    print("| 閾値 | 光った誤変換文 | うち誤り箇所に重なる |")
    print("|---|---|---|")
    for threshold: Float in [0.5, 1.0, 1.5, 2.0, 3.0] {
        var flagged = 0, localized = 0
        for item in errorItems {
            let lit = item.confidences.enumerated().filter { $0.element.margin < threshold }.map(\.offset)
            guard !lit.isEmpty else { continue }
            flagged += 1
            let errors = item.mask.enumerated().filter(\.element).map(\.offset)
            if lit.contains(where: { l in errors.contains { abs($0 - l) <= 1 } }) { localized += 1 }
        }
        print("| \(threshold) | \(flagged) | \(percent(localized, flagged)) |")
    }
    print()

    // 例: 誤変換文と正しい文をそれぞれ数件、閾値で光る文字を【】で囲んで示す
    func marked(_ item: ItemRecord, threshold: Float) -> String {
        var result = ""
        var inside = false
        for (character, confidence) in zip(item.output, item.confidences) {
            let lit = confidence.margin < threshold
            if lit != inside { result += lit ? "【" : "】"; inside = lit }
            result.append(character)
        }
        if inside { result += "】" }
        return result
    }
    print("## 例（マージン < \(exampleThreshold) を【】で囲む）")
    print()
    print("誤変換文:")
    for item in errorItems.prefix(exampleCount) {
        print("  [\(item.index)] \(marked(item, threshold: exampleThreshold))")
        print("        正解: \(item.references.joined(separator: " / "))")
    }
    print("正しい文:")
    for item in correctItems.prefix(exampleCount) {
        print("  [\(item.index)] \(marked(item, threshold: exampleThreshold))")
    }

case "lattice" where arguments.count >= 3:
    // 辞書ラティス（azooKey）の生の候補を見る（調査用）。★は読み全体に一致した候補。
    // モデルがあれば各候補のzenz採点（対数確率）と、zenzの自由生成の結果も並べる
    //   iroha-cli lattice [--context 文脈] <読み>
    guard let dictionaryURL = LatticeConverter.defaultDictionaryURL() else {
        FileHandle.standardError.write("辞書が見つかりません（scripts/fetch-dictionary.sh で取得できます）\n".data(using: .utf8)!)
        exit(1)
    }
    var context = ""
    var readingArgument: String?
    var index = 2
    while index < arguments.count {
        if arguments[index] == "--context", index + 1 < arguments.count {
            context = arguments[index + 1]
            index += 2
        } else {
            readingArgument = arguments[index]
            index += 1
        }
    }
    let lattice = LatticeConverter(dictionaryURL: dictionaryURL)
    let reading = romajiToKana(readingArgument ?? "")
    let start = ContinuousClock.now
    let candidates = await lattice.rawCandidates(reading: reading, count: 20)
    let elapsed = start.duration(to: .now)
    print("\(reading)  [\(elapsed.components.attoseconds / 1_000_000_000_000_000 + elapsed.components.seconds * 1000)ms]")
    let zenz = makeZenz()
    let full = candidates.filter(\.isFullMatch).map(\.text)
    var scored: [String: Float] = [:]
    var generated: String?
    if (try? await zenz.prewarm()) != nil {
        generated = try? await zenz.convert(reading: reading, context: context, candidateCount: 1).first
        var toScore = full
        if let generated, !toScore.contains(generated) { toScore.append(generated) }
        if let scores = try? await zenz.score(candidates: toScore, reading: reading, context: context) {
            for (text, score) in zip(toScore, scores) { scored[text] = score }
        }
    }
    if let generated {
        print(String(format: "  zenz生成: %@  (%.2f)", generated, scored[generated] ?? .nan))
    }
    for candidate in candidates {
        let score = scored[candidate.text].map { String(format: "%8.2f", $0) } ?? "        "
        print(String(format: "  %@ %8.2f %@  %@", candidate.isFullMatch ? "★" : "　", candidate.value, score, candidate.text))
    }

case "predict" where arguments.count >= 3:
    // 予測変換・インライン補完の検証: 左文脈の続き（1文節）を生成する。
    // --chain N で予測をTabで取り入れ続けた場合の見た目（N回ぶん）を出す
    var chain = 1
    var contextArgument: String?
    var index = 2
    while index < arguments.count {
        if arguments[index] == "--chain", index + 1 < arguments.count {
            chain = max(1, Int(arguments[index + 1]) ?? 1)
            index += 2
        } else {
            contextArgument = arguments[index]
            index += 1
        }
    }
    guard var context = contextArgument else {
        FileHandle.standardError.write("使い方: iroha-cli predict [--chain 回数] <左文脈>\n".data(using: .utf8)!)
        exit(1)
    }
    let zenz = makeZenz()
    do {
        try await zenz.prewarm()
        for _ in 0..<chain {
            let start = ContinuousClock.now
            let prediction = try await zenz.predict(context: context, maxLength: 16)
            let elapsed = start.duration(to: .now)
            let ms = Double(elapsed.components.attoseconds) / 1e15 + Double(elapsed.components.seconds) * 1e3
            print("\(context) -> [\(prediction)]  [\(String(format: "%.1f", ms))ms]")
            guard !prediction.isEmpty else { break }
            context += prediction
        }
    } catch {
        FileHandle.standardError.write("エラー: \(error)\n".data(using: .utf8)!)
    }

case "typo":
    // 打ち間違い訂正モデル（training/typo-normalizer、SWIFT-PORT.md）の検証。
    //   typo parity [--dir DIR]          書き出しに付いてくる parity.json 200件と突き合わせる
    //   typo bench <test.jsonl> [--n 件数] レイテンシ（mean / p50 / p95）
    //   typo <読み> [--threshold θ]       1件だけ試す
    var typoDirectory = TypoNormalizer.defaultDirectoryURL()
    var typoThreshold = TypoNormalizer.defaultThreshold
    var typoCount = 300
    var typoPositional: [String] = []
    var typoIndex = 2
    while typoIndex < arguments.count {
        switch arguments[typoIndex] {
        case "--dir" where typoIndex + 1 < arguments.count:
            typoDirectory = URL(fileURLWithPath: arguments[typoIndex + 1], isDirectory: true)
            typoIndex += 2
        case "--threshold" where typoIndex + 1 < arguments.count:
            typoThreshold = Double(arguments[typoIndex + 1]) ?? typoThreshold
            typoIndex += 2
        case "--n" where typoIndex + 1 < arguments.count:
            typoCount = Int(arguments[typoIndex + 1]) ?? typoCount
            typoIndex += 2
        default:
            typoPositional.append(arguments[typoIndex])
            typoIndex += 1
        }
    }
    guard let typoDirectory else {
        FileHandle.standardError.write(
            "Typo Normalizer のモデルが見つかりません（IROHA_TYPO_MODEL か --dir で指定してください）\n"
                .data(using: .utf8)!)
        exit(1)
    }
    let normalizer = TypoNormalizer(directory: typoDirectory)

    switch typoPositional.first {
    case "parity":
        // SWIFT-PORT.md §7 の検証。ロジット → 生成 → logP の順に見る
        // （生成が一致してもロジットがずれていれば実装は間違っている）
        struct ParityCase: Decodable {
            let noisy: String
            let clean: String
            let greedy: String
            let logprobGreedy: Double
            /// 入力を出力できない（出力語彙に無い文字を含む）ときは null。Swift 側は −∞ を返すはず
            let logprobNoisy: Double?
            let margin: Double?
            let firstLogits: [[Float]]
            enum CodingKeys: String, CodingKey {
                case noisy, clean, greedy, margin
                case logprobGreedy = "logprob_greedy"
                case logprobNoisy = "logprob_noisy"
                case firstLogits = "first_logits"
            }
        }
        struct ParityFile: Decodable {
            let run: String
            let cases: [ParityCase]
        }
        let parityURL = typoDirectory.appendingPathComponent("parity.json")
        guard let parityData = try? Data(contentsOf: parityURL),
              let parity = try? JSONDecoder().decode(ParityFile.self, from: parityData) else {
            FileHandle.standardError.write("parity.json が読めません: \(parityURL.path)\n".data(using: .utf8)!)
            exit(1)
        }
        var logitError: Float = 0
        var greedyMatches = 0
        var logProbError = 0.0
        var marginError = 0.0
        var mismatches: [String] = []
        var unrepresentable = 0          // 入力そのものを出力できない例（期待値が null）
        var unrepresentableMismatch = 0  // そのうち Swift が −∞ を返さなかったもの
        do {
            for item in parity.cases {
                // ① teacher forcing のロジット（先頭3ステップ）
                let logits = try await normalizer.logits(
                    source: item.noisy, target: item.clean, steps: item.firstLogits.count)
                for (step, expected) in item.firstLogits.enumerated() where step < logits.count {
                    for (index, value) in expected.enumerated() {
                        logitError = max(logitError, abs(value - logits[step][index]))
                    }
                }
                // ② greedy
                let generated = try await normalizer.generate(for: item.noisy) ?? ""
                if generated == item.greedy {
                    greedyMatches += 1
                } else if mismatches.count < 10 {
                    mismatches.append("  \(item.noisy) → \(generated)（期待 \(item.greedy)）")
                }
                // ③ logP と margin
                let greedyLogProb = try await normalizer.logProbability(of: item.greedy, given: item.noisy)
                let noisyLogProb = try await normalizer.logProbability(of: item.noisy, given: item.noisy)
                logProbError = max(logProbError, abs(greedyLogProb - item.logprobGreedy))
                if let expectedNoisy = item.logprobNoisy, let expectedMargin = item.margin {
                    logProbError = max(logProbError, abs(noisyLogProb - expectedNoisy))
                    marginError = max(marginError, abs((greedyLogProb - noisyLogProb) - expectedMargin))
                } else {
                    unrepresentable += 1
                    if noisyLogProb != -.infinity { unrepresentableMismatch += 1 }
                }
            }
        } catch {
            FileHandle.standardError.write("エラー: \(error)\n".data(using: .utf8)!)
            exit(1)
        }
        // 許容幅は重みの精度で変える（SWIFT-PORT.md §7）。float32 は「ロジット 1e-3・logP 0.01」、
        // float16 に落としたあとは「greedy が数件ずれるのは許容・margin が 0.1 以上ずれたら戻す」
        let isHalf = ((try? Data(contentsOf: typoDirectory.appendingPathComponent("manifest.json")))
            .flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] }?["dtype"]
            as? String) == "float16-le"
        let logitTolerance: Float = isHalf ? 0.05 : 1e-3
        let logProbTolerance = isHalf ? 0.1 : 0.01
        let greedyFloor = isHalf ? parity.cases.count - parity.cases.count / 50 : parity.cases.count
        let logitOK = logitError <= logitTolerance
        let greedyOK = greedyMatches >= greedyFloor
        let logProbOK = logProbError <= logProbTolerance && marginError <= logProbTolerance
            && unrepresentableMismatch == 0
        print("parity: \(parity.run) / \(parity.cases.count)件  (\(typoDirectory.path))")
        print("  重みの精度         \(isHalf ? "float16" : "float32")")
        print("  ロジット最大誤差   \(String(format: "%.6f", logitError))   \(logitOK ? "OK" : "NG")（許容 \(logitTolerance)）")
        print("  greedy 一致        \(greedyMatches)/\(parity.cases.count)   \(greedyOK ? "OK" : "NG")（許容 \(greedyFloor)以上）")
        print("  logP 最大誤差      \(String(format: "%.6f", logProbError))   \(logProbOK ? "OK" : "NG")（許容 \(logProbTolerance)）")
        print("  margin 最大誤差    \(String(format: "%.6f", marginError))")
        if unrepresentable > 0 {
            print("  入力を出力できない \(unrepresentable)件（logP −∞ でないもの \(unrepresentableMismatch)件）")
        }
        for line in mismatches { print(line) }
        exit(logitOK && greedyOK && logProbOK ? 0 : 1)

    case "shrink":
        // float32 の書き出しを float16 に落として半分にする（12.8MB → 6.4MB）。
        // 落としたあとは必ず parity をもう一度回すこと（SWIFT-PORT.md §7-5。
        // greedy が数件ずれるのは許容、margin が 0.1 以上ずれるなら float32 に戻す）
        guard typoPositional.count >= 2 else {
            FileHandle.standardError.write(
                "使い方: iroha-cli typo shrink <出力ディレクトリ>\n".data(using: .utf8)!)
            exit(1)
        }
        let destination = URL(fileURLWithPath: typoPositional[1], isDirectory: true)
        do {
            let manifestURL = typoDirectory.appendingPathComponent("manifest.json")
            let manifestData = try Data(contentsOf: manifestURL)
            guard var manifestObject = try JSONSerialization.jsonObject(with: manifestData)
                    as? [String: Any],
                  let totalFloats = manifestObject["total_floats"] as? Int,
                  manifestObject["dtype"] as? String == "float32-le" else {
                FileHandle.standardError.write("float32-le の manifest ではありません\n".data(using: .utf8)!)
                exit(1)
            }
            let source = try Data(contentsOf: typoDirectory.appendingPathComponent("weights.bin"))
            guard source.count == totalFloats * 4 else {
                let message = "weights.bin の大きさが manifest と合いません"
                    + "（期待 \(totalFloats * 4) バイト、実際 \(source.count) バイト）\n"
                FileHandle.standardError.write(message.data(using: .utf8)!)
                exit(1)
            }
            var halves = [UInt16](repeating: 0, count: totalFloats)
            source.withUnsafeBytes { raw in
                let floats = raw.bindMemory(to: Float.self)
                for i in 0..<totalFloats { halves[i] = TypoWeightConversion.floatToHalf(floats[i]) }
            }
            manifestObject["dtype"] = "float16-le"
            try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)
            try JSONSerialization.data(withJSONObject: manifestObject, options: [.prettyPrinted, .sortedKeys])
                .write(to: destination.appendingPathComponent("manifest.json"))
            try halves.withUnsafeBufferPointer { Data(buffer: $0) }
                .write(to: destination.appendingPathComponent("weights.bin"))
            // parity.json はそのまま（float32 で作った期待値と比べるのが目的）
            let parity = typoDirectory.appendingPathComponent("parity.json")
            if FileManager.default.fileExists(atPath: parity.path) {
                try? FileManager.default.removeItem(at: destination.appendingPathComponent("parity.json"))
                try FileManager.default.copyItem(at: parity, to: destination.appendingPathComponent("parity.json"))
            }
            print("float16 で書き出しました: \(destination.path)  (\(totalFloats * 2 / 1_000_000)MB)")
            print("次に: iroha-cli typo parity --dir \(destination.path)")
        } catch {
            FileHandle.standardError.write("エラー: \(error)\n".data(using: .utf8)!)
            exit(1)
        }

    case "prefix":
        // 「入力途中の読み」に対して訂正を出してしまう割合を測る。
        // 合成中（確定前）に走らせる設計が成り立つかの判断材料（SWIFT-PORT.md §5 は
        // 「打ち終わった読みでしか学習していないので入力途中を typo と誤認する」と警告している）。
        //   正しい読みを途中で切ったもの  → 訂正を出したら誤検出
        //   typo のある読みを途中で切ったもの → typo を含む長さなら直せてほしい
        guard typoPositional.count >= 2 else {
            FileHandle.standardError.write(
                "使い方: iroha-cli typo prefix <test.jsonl> [--n 件数] [--threshold θ]\n".data(using: .utf8)!)
            exit(1)
        }
        guard let content = try? String(contentsOfFile: typoPositional[1], encoding: .utf8) else {
            FileHandle.standardError.write("ファイルが読めません: \(typoPositional[1])\n".data(using: .utf8)!)
            exit(1)
        }
        struct PrefixRecord: Decodable {
            let noisy: String
            let clean: String
            let errorType: String
            enum CodingKeys: String, CodingKey {
                case noisy, clean
                case errorType = "error_type"
            }
        }
        let prefixDecoder = JSONDecoder()
        let records: [PrefixRecord] = content.split(separator: "\n").prefix(typoCount).compactMap {
            guard let data = $0.data(using: .utf8) else { return nil }
            return try? prefixDecoder.decode(PrefixRecord.self, from: data)
        }
        guard !records.isEmpty else {
            FileHandle.standardError.write("評価できる行がありません\n".data(using: .utf8)!)
            exit(1)
        }
        let fractions = [0.25, 0.5, 0.75, 1.0]
        var cleanTotal = [Int](repeating: 0, count: fractions.count)
        var cleanFired = [Int](repeating: 0, count: fractions.count)
        var typoTotal = [Int](repeating: 0, count: fractions.count)
        var typoFixed = [Int](repeating: 0, count: fractions.count)
        var typoWrong = [Int](repeating: 0, count: fractions.count)
        do {
            for record in records {
                let noisy = Array(record.noisy)
                let clean = Array(record.clean)
                for (slot, fraction) in fractions.enumerated() {
                    let cut = max(1, Int((Double(noisy.count) * fraction).rounded()))
                    guard cut <= noisy.count else { continue }
                    let partial = String(noisy[0..<cut])
                    guard let correction = try await normalizer.correction(
                        for: partial, threshold: typoThreshold) else {
                        if record.errorType == "none" { cleanTotal[slot] += 1 } else { typoTotal[slot] += 1 }
                        continue
                    }
                    if record.errorType == "none" {
                        // 正しい読みの途中なので、何か出したら誤検出
                        cleanTotal[slot] += 1
                        cleanFired[slot] += 1
                    } else {
                        // typo 側: 切った長さに対応する正解の前半と比べる。
                        // typo は 1 文字ぶん長さを変えるので（重複・「っ」の過不足）、
                        // 全部打ち終わっていれば正解そのもの、途中なら正解の前半と前方一致で甘く見る
                        typoTotal[slot] += 1
                        let expected = cut == noisy.count
                            ? record.clean : String(clean[0..<min(cut, clean.count)])
                        if correction.corrected == expected
                            || expected.hasPrefix(correction.corrected)
                            || correction.corrected.hasPrefix(expected) {
                            typoFixed[slot] += 1
                        } else {
                            typoWrong[slot] += 1
                        }
                    }
                }
            }
        } catch {
            FileHandle.standardError.write("エラー: \(error)\n".data(using: .utf8)!)
            exit(1)
        }
        print("typo prefix: \(records.count)件 × 読みの長さ \(fractions.map { Int($0 * 100) })%  θ=\(typoThreshold)")
        print("  読みの割合   正しい読みへの誤検出        typoを直せた        typoに別の訂正")
        for (slot, fraction) in fractions.enumerated() {
            let falseRate = cleanTotal[slot] == 0 ? 0
                : Double(cleanFired[slot]) / Double(cleanTotal[slot]) * 100
            let fixRate = typoTotal[slot] == 0 ? 0
                : Double(typoFixed[slot]) / Double(typoTotal[slot]) * 100
            let wrongRate = typoTotal[slot] == 0 ? 0
                : Double(typoWrong[slot]) / Double(typoTotal[slot]) * 100
            print(String(format: "  %4d%%        %6.2f%% (%d/%d)   %6.2f%% (%d/%d)   %6.2f%%",
                         Int(fraction * 100), falseRate, cleanFired[slot], cleanTotal[slot],
                         fixRate, typoFixed[slot], typoTotal[slot], wrongRate))
        }

    case "segments":
        // 採用した訂正が「1文節の中に収まるか」を実際の変換・文節分割で測る。
        // 収まらない（差分が文節境界をまたぐ）ときは今の作りでは候補を出せないので、
        // その割合が「文節分割のせいで訂正を出せない率」になる
        guard typoPositional.count >= 2 else {
            FileHandle.standardError.write(
                "使い方: iroha-cli typo segments <test.jsonl> [--n 件数] [--threshold θ]\n".data(using: .utf8)!)
            exit(1)
        }
        guard let content = try? String(contentsOfFile: typoPositional[1], encoding: .utf8) else {
            FileHandle.standardError.write("ファイルが読めません: \(typoPositional[1])\n".data(using: .utf8)!)
            exit(1)
        }
        struct SegRecord: Decodable {
            let noisy: String
            let clean: String
            let errorType: String
            enum CodingKeys: String, CodingKey {
                case noisy, clean
                case errorType = "error_type"
            }
        }
        let segDecoder = JSONDecoder()
        let segRecords: [SegRecord] = content.split(separator: "\n").prefix(typoCount).compactMap {
            guard let data = $0.data(using: .utf8) else { return nil }
            return try? segDecoder.decode(SegRecord.self, from: data)
        }
        let segEngine = makeEngine()
        var accepted = 0, fitsInSegment = 0, straddles = 0, singleSegment = 0
        var wholeSentenceCorrect = 0, segmentCorrect = 0
        var examples: [String] = []
        do {
            for record in segRecords where record.errorType != "none" {
                guard let correction = try await normalizer.correction(
                    for: record.noisy, threshold: typoThreshold) else { continue }
                accepted += 1
                // IME と同じ手順: typo のある読みをそのまま変換して文節に割る
                let conversion = try await segEngine.convert(
                    reading: record.noisy, context: "", candidateCount: 1).first ?? record.noisy
                let segments = ReadingAligner.segmentReading(record.noisy, conversion: conversion)
                let readings = segments.map(\.reading)
                if readings.count == 1 { singleSegment += 1 }
                if let placement = correction.placement(inSegments: readings) {
                    fitsInSegment += 1
                    // その文節だけ直した読み全体が正解と一致するか
                    var fixed = readings
                    fixed[placement.index] = placement.correctedReading
                    if fixed.joined() == record.clean { segmentCorrect += 1 }
                } else {
                    straddles += 1
                    if examples.count < 6 {
                        examples.append("  \(record.noisy) → \(correction.corrected)"
                            + "  文節: \(readings.joined(separator: "|"))")
                    }
                }
                if correction.corrected == record.clean { wholeSentenceCorrect += 1 }
            }
        } catch {
            FileHandle.standardError.write("エラー: \(error)\n".data(using: .utf8)!)
            exit(1)
        }
        print("typo segments: typoのある \(segRecords.filter { $0.errorType != "none" }.count) 件  θ=\(typoThreshold)")
        print("  訂正を採用            \(accepted)")
        print(String(format: "  1文節に収まる        %d (%.1f%%) ← 今の作りで候補を出せる",
                     fitsInSegment, Double(fitsInSegment) / Double(max(1, accepted)) * 100))
        print(String(format: "  文節境界をまたぐ      %d (%.1f%%) ← 今の作りでは何も出せない",
                     straddles, Double(straddles) / Double(max(1, accepted)) * 100))
        print("  （うち1文節しかない   \(singleSegment)）")
        print(String(format: "  読み全体として正解    %d (%.1f%%)",
                     wholeSentenceCorrect, Double(wholeSentenceCorrect) / Double(max(1, accepted)) * 100))
        if !examples.isEmpty {
            print("  またいだ例:")
            for line in examples { print(line) }
        }

    case "pause":
        // 「人が入力を止めそうな場所」で切った読みに訂正を出してしまう割合。
        //
        // `typo prefix` は文字数の 25/50/75% という機械的な位置で切るので、語の途中が多く
        // 条件が実際より厳しい。人が 300ms 止まるのは文節や句の切れ目なので、ここでは
        // 文節の境目（= 変換・文節分割が見ている切れ目）で切って測る。
        // 合成中に訂正を走らせる設計（入力の休止で読みを直す）の誤検出率はこちらが近い
        guard typoPositional.count >= 2 else {
            FileHandle.standardError.write(
                "使い方: iroha-cli typo pause <test.jsonl> [--n 件数] [--threshold θ]\n".data(using: .utf8)!)
            exit(1)
        }
        guard let content = try? String(contentsOfFile: typoPositional[1], encoding: .utf8) else {
            FileHandle.standardError.write("ファイルが読めません: \(typoPositional[1])\n".data(using: .utf8)!)
            exit(1)
        }
        struct PauseRecord: Decodable {
            let noisy: String
            let clean: String
            let errorType: String
            enum CodingKeys: String, CodingKey {
                case noisy, clean
                case errorType = "error_type"
            }
        }
        let pauseDecoder = JSONDecoder()
        let pauseRecords: [PauseRecord] = content.split(separator: "\n").prefix(typoCount).compactMap {
            guard let data = $0.data(using: .utf8) else { return nil }
            return try? pauseDecoder.decode(PauseRecord.self, from: data)
        }
        // --keep-tail を付けると素の挙動（末尾への追加も誤検出に数える）
        let dropTrailingInsertions = !typoPositional.contains("--keep-tail")
        let pauseEngine = makeEngine()
        // 正しく打てている読みだけを見る（誤検出＝訂正を出したら負け）
        let cleanRecords = pauseRecords.filter { $0.errorType == "none" }
        var boundaryTotal = 0, boundaryFired = 0
        var finalTotal = 0, finalFired = 0
        var examples: [String] = []
        do {
            for record in cleanRecords {
                let conversion = try await pauseEngine.convert(
                    reading: record.clean, context: "", candidateCount: 1).first ?? record.clean
                let segments = ReadingAligner.segmentReading(record.clean, conversion: conversion)
                var prefix = ""
                for (index, segment) in segments.enumerated() {
                    prefix += segment.reading
                    let isFinal = index == segments.count - 1
                    var correction = try await normalizer.correction(
                        for: prefix, threshold: typoThreshold)
                    // 入力中の訂正と同じ条件: 末尾に足しただけのものは捨てる
                    if dropTrailingInsertions, correction?.isTrailingInsertionOnly == true {
                        correction = nil
                    }
                    if isFinal {
                        finalTotal += 1
                        if correction != nil { finalFired += 1 }
                    } else {
                        boundaryTotal += 1
                        if let correction {
                            boundaryFired += 1
                            if examples.count < 5 {
                                examples.append("  \(prefix) → \(correction.corrected)"
                                    + String(format: "  (margin %.1f)", correction.margin))
                            }
                        }
                    }
                }
            }
        } catch {
            FileHandle.standardError.write("エラー: \(error)\n".data(using: .utf8)!)
            exit(1)
        }
        print("typo pause: 正しく打てている \(cleanRecords.count) 件  θ=\(typoThreshold)"
            + (dropTrailingInsertions ? "  末尾への追加は捨てる" : "  末尾への追加も数える"))
        print(String(format: "  文の途中の文節境界で切った  %5.2f%% が誤検出 (%d/%d)",
                     boundaryTotal == 0 ? 0 : Double(boundaryFired) / Double(boundaryTotal) * 100,
                     boundaryFired, boundaryTotal))
        print(String(format: "  最後まで打ち終わった時点    %5.2f%% が誤検出 (%d/%d)",
                     finalTotal == 0 ? 0 : Double(finalFired) / Double(finalTotal) * 100,
                     finalFired, finalTotal))
        if !examples.isEmpty {
            print("  誤検出の例:")
            for line in examples { print(line) }
        }

    case "catalog":
        // 配布中のモデル一覧を見る / 取得する。設定画面のダウンロードと同じ経路を通るので、
        // 公開したカタログとリリースが正しいかをここで確かめられる
        //   typo catalog            一覧を表示
        //   typo catalog install    先頭（推奨）のモデルを取得して設置
        //   typo catalog install <ID>
        //   typo catalog remove     設置したモデルを削除
        let action = typoPositional.count >= 2 ? typoPositional[1] : "list"
        do {
            switch action {
            case "remove":
                try TypoNormalizerInstall.remove()
                print("設置したモデルを削除しました")
            case "list", "install":
                let catalog = try await TypoNormalizerFetcher.fetchCatalog()
                print("カタログ: \(TypoNormalizerCatalog.defaultURL)")
                if let license = catalog.license { print("  既定ライセンス: \(license)") }
                let installed = TypoNormalizerInstall.installedRecord()
                for model in catalog.models {
                    let mark = installed?.id == model.id ? " ← 設置済み" : ""
                    print(String(format: "  %-12s %@  %.1fMB%@", (model.id as NSString).utf8String!,
                                 model.name, Double(model.totalBytes) / 1_000_000, mark))
                    if let summary = model.summary { print("               \(summary)") }
                    if let license = catalog.license(for: model) {
                        print("               \(license)"
                            + (catalog.attribution(for: model).map { " / 学習元: \($0)" } ?? ""))
                    }
                }
                guard action == "install" else { break }
                let target: TypoNormalizerCatalog.Model?
                if typoPositional.count >= 3 {
                    target = catalog.model(id: typoPositional[2])
                    if target == nil {
                        FileHandle.standardError.write(
                            "そのIDのモデルがありません: \(typoPositional[2])\n".data(using: .utf8)!)
                        exit(1)
                    }
                } else {
                    target = catalog.models.first
                }
                guard let target else { exit(1) }
                print("\n取得します: \(target.id)")
                let record = try await TypoNormalizerFetcher.install(target) { progress in
                    // 進捗は別スレッドから来る。10%刻みだけ出す
                    ProgressPrinter.shared.report(progress)
                }
                print("設置しました: \(record.name) (\(record.id))")
                print("  場所: \(TypoNormalizerInstall.directoryURL.path)")
            default:
                FileHandle.standardError.write(
                    "使い方: iroha-cli typo catalog [list|install [ID]|remove]\n".data(using: .utf8)!)
                exit(1)
            }
        } catch {
            FileHandle.standardError.write("エラー: \(error.localizedDescription)\n".data(using: .utf8)!)
            exit(1)
        }

    case "eval":
        // threshold_curve.py と同じ表を Swift 側で出す（移植の最終確認と、モデルを差し替えたときの再測定）
        guard typoPositional.count >= 2 else {
            FileHandle.standardError.write(
                "使い方: iroha-cli typo eval <test.jsonl> [--n 件数]\n".data(using: .utf8)!)
            exit(1)
        }
        guard let content = try? String(contentsOfFile: typoPositional[1], encoding: .utf8) else {
            FileHandle.standardError.write("ファイルが読めません: \(typoPositional[1])\n".data(using: .utf8)!)
            exit(1)
        }
        struct TypoRecord: Decodable {
            let noisy: String
            let clean: String
            let errorType: String
            enum CodingKeys: String, CodingKey {
                case noisy, clean
                case errorType = "error_type"
            }
        }
        let decoder = JSONDecoder()
        let records: [TypoRecord] = content.split(separator: "\n").prefix(typoCount).compactMap {
            guard let data = $0.data(using: .utf8) else { return nil }
            return try? decoder.decode(TypoRecord.self, from: data)
        }
        guard !records.isEmpty else {
            FileHandle.standardError.write("評価できる行がありません\n".data(using: .utf8)!)
            exit(1)
        }
        // 生成と margin は θ に依らないので 1 回だけ計算して、θ を振るのは採否の判定だけにする
        var predictions: [String] = []
        var margins: [Double] = []
        var unsupported = 0
        do {
            for record in records {
                // 本体の correction(for:) と同じく、語彙外の文字を含む・長すぎる読みは素通しにする
                // （通さないと「入力そのまま」の logP が −∞ になり、必ず書き換えたことになる）
                guard try await normalizer.supports(reading: record.noisy) else {
                    unsupported += 1
                    predictions.append(record.noisy)
                    margins.append(0)
                    continue
                }
                let generated = try await normalizer.generate(for: record.noisy) ?? record.noisy
                predictions.append(generated)
                if generated == record.noisy {
                    margins.append(0)
                } else {
                    margins.append(
                        try await normalizer.logProbability(of: generated, given: record.noisy)
                            - normalizer.logProbability(of: record.noisy, given: record.noisy))
                }
            }
        } catch {
            FileHandle.standardError.write("エラー: \(error)\n".data(using: .utf8)!)
            exit(1)
        }
        let cleanCount = records.filter { $0.errorType == "none" }.count
        let typoCountTotal = records.count - cleanCount
        let characters = records.reduce(0) { $0 + $1.clean.count }
        print("typo eval: \(records.count)件（正しい入力 \(cleanCount) / typo \(typoCountTotal)）  \(typoDirectory.path)")
        if unsupported > 0 {
            print("（語彙外の文字を含む・長すぎるため素通しにした入力: \(unsupported) 件）")
        }
        print(" θ        Exact Match   CER    Typo訂正率   過剰訂正率   書き換えた割合")
        for threshold in [-Double.infinity, 0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0, 10.0] {
            var exact = 0, errors = 0, changed = 0, typoFixed = 0, falseCorrections = 0
            for (index, record) in records.enumerated() {
                let prediction = predictions[index]
                let final = (prediction != record.noisy && margins[index] > threshold)
                    ? prediction : record.noisy
                if final == record.clean { exact += 1 }
                errors += editDistance(Array(final), Array(record.clean))
                if final != record.noisy { changed += 1 }
                if record.errorType == "none" {
                    if final != record.clean { falseCorrections += 1 }
                } else if final == record.clean {
                    typoFixed += 1
                }
            }
            let label = threshold == -.infinity ? "なし" : String(format: "%.1f", threshold)
            print(String(
                format: " %6@   %7.2f%%  %5.2f%%   %7.2f%%   %7.2f%%   %6.2f%%",
                label as NSString,
                Double(exact) / Double(records.count) * 100,
                Double(errors) / Double(max(1, characters)) * 100,
                typoCountTotal == 0 ? 0 : Double(typoFixed) / Double(typoCountTotal) * 100,
                cleanCount == 0 ? 0 : Double(falseCorrections) / Double(cleanCount) * 100,
                Double(changed) / Double(records.count) * 100))
        }

    case "bench":
        guard typoPositional.count >= 2 else {
            FileHandle.standardError.write("使い方: iroha-cli typo bench <test.jsonl> [--n 件数]\n".data(using: .utf8)!)
            exit(1)
        }
        guard let content = try? String(contentsOfFile: typoPositional[1], encoding: .utf8) else {
            FileHandle.standardError.write("ファイルが読めません: \(typoPositional[1])\n".data(using: .utf8)!)
            exit(1)
        }
        // JSONL（{"noisy": ...}）でも、1行1読みのテキストでも受ける
        let readings: [String] = content.split(separator: "\n").prefix(typoCount).compactMap { line in
            guard let data = line.data(using: .utf8),
                  let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
            else { return String(line) }
            return object["noisy"] as? String
        }
        guard !readings.isEmpty else {
            FileHandle.standardError.write("測れる読みがありません: \(typoPositional[1])\n".data(using: .utf8)!)
            exit(1)
        }
        do {
            try await normalizer.prewarm()
            _ = try await normalizer.generate(for: readings.first ?? "てすと")   // 初回の確保を測らない
            var times: [Double] = []
            var corrections = 0
            for reading in readings {
                let start = ContinuousClock.now
                // 実運用と同じ経路（生成1回 + 前向き2回）を測る
                let correction = try await normalizer.correction(for: reading, threshold: typoThreshold)
                let elapsed = start.duration(to: .now)
                times.append(Double(elapsed.components.attoseconds) / 1e15
                    + Double(elapsed.components.seconds) * 1e3)
                if correction != nil { corrections += 1 }
            }
            times.sort()
            func percentile(_ p: Double) -> Double { times[min(times.count - 1, Int(Double(times.count) * p))] }
            let mean = times.reduce(0, +) / Double(times.count)
            print("typo bench: \(times.count)件  θ=\(typoThreshold)")
            print(String(format: "  mean %.2fms  p50 %.2fms  p95 %.2fms  max %.2fms",
                         mean, percentile(0.5), percentile(0.95), times.last ?? 0))
            print("  訂正を出した件数 \(corrections)/\(times.count)")
            print("  目標は mean 7ms 以下・p95 15ms 以下（SWIFT-PORT.md §7）")
        } catch {
            FileHandle.standardError.write("エラー: \(error)\n".data(using: .utf8)!)
            exit(1)
        }

    case .some(let reading):
        do {
            let kana = romajiToKana(reading)
            let generated = try await normalizer.generate(for: kana)
            let correction = try await normalizer.correction(for: kana, threshold: typoThreshold)
            print("入力     \(kana)")
            print("生成     \(generated ?? "（対象外）")")
            if let correction {
                print(String(format: "採用     %@  (margin %.3f > θ %.2f)",
                             correction.corrected, correction.margin, typoThreshold))
                if correction.isTrailingInsertionOnly {
                    print("         ただし末尾に足しただけなので、入力中の訂正では捨てられる"
                        + "（打ちかけの読みは常に終わりが足りなく見えるため）")
                }
            } else if let generated, generated != kana {
                let margin = try await normalizer.logProbability(of: generated, given: kana)
                    - normalizer.logProbability(of: kana, given: kana)
                print(String(format: "不採用   margin %.3f ≤ θ %.2f", margin, typoThreshold))
            } else {
                print("不採用   訂正なし")
            }
        } catch {
            FileHandle.standardError.write("エラー: \(error)\n".data(using: .utf8)!)
            exit(1)
        }

    case .none:
        FileHandle.standardError.write(
            "使い方: iroha-cli typo parity | typo bench <test.jsonl> | typo <読み> [--threshold θ]\n"
                .data(using: .utf8)!)
        exit(1)
    }

case "convert":
    var context = ""
    var count = 1
    var reading: String?
    var index = 2
    while index < arguments.count {
        if arguments[index] == "--context", index + 1 < arguments.count {
            context = arguments[index + 1]
            index += 2
        } else if arguments[index] == "--n", index + 1 < arguments.count {
            count = Int(arguments[index + 1]) ?? 1
            index += 2
        } else {
            reading = arguments[index]
            index += 1
        }
    }
    guard let reading else {
        FileHandle.standardError.write("使い方: iroha-cli convert [--context 文脈] [--n 候補数] <読み>\n".data(using: .utf8)!)
        exit(1)
    }
    let engine = makeEngine()
    await convertAndPrint(engine: engine, reading: reading, context: context, count: count)

default:  // repl
    let engine = makeEngine()
    FileHandle.standardError.write("読みを入力してください（Ctrl-Dで終了）\n".data(using: .utf8)!)
    while let line = readLine(), !line.isEmpty {
        await convertAndPrint(engine: engine, reading: line, context: "")
    }
}
