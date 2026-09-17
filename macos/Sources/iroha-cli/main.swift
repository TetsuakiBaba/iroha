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
//   iroha-cli confidence <evaluation_items.json> [--margin 閾値] [--examples 件数]
//                                                 : 自信度（文字ごとのマージン）と誤変換箇所の対応を
//                                                   AJIMEE-Benchで評価（zenz単体。誤変換検出の閾値設計用）
//   iroha-cli repl                                : 対話モード（1行ずつ変換、レイテンシ表示）
//   iroha-cli lattice <読み>                       : 辞書ラティス（azooKey）の生の候補を表示（調査用）
//   iroha-cli predict [--chain 回数] <左文脈>        : 予測（左文脈の続き1文節）。--chainで採用を繰り返す
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
