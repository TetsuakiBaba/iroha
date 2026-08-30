import Foundation
import IrohaCore

// 変換エンジンの検証用CLIハーネス。
//
// 使い方:
//   iroha-cli kana <romaji>                       : ローマ字→かな変換のみ
//   iroha-cli convert [--context 文脈] <読み>      : かな漢字変換（読みはローマ字/ひらがなどちらでも）
//   iroha-cli segment <読み>                       : 変換 + 文節分割の検証
//   iroha-cli bench <eval.tsv>                    : 評価（TSV: 読み\t正解）。精度とレイテンシを報告
//   iroha-cli ajimee <evaluation_items.json>      : AJIMEE-Bench評価（acc@1・MinCER）。scripts/fetch-ajimee.shで取得
//   iroha-cli repl                                : 対話モード（1行ずつ変換、レイテンシ表示）
//   環境変数 IROHA_MODEL でモデルパス、IROHA_USER_DICT でユーザ辞書、
//   IROHA_LEARNING で学習結果のファイルを上書き可能

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

/// IME本体と同じ構成（zenz + ユーザ辞書）でエンジンを組み立てる。
/// IROHA_USER_DICT でユーザ辞書のJSONを差し替えられる（既定は本体と同じファイル）
func makeEngine() -> any ConversionEngine {
    let zenz: ZenzEngine
    if let path = ProcessInfo.processInfo.environment["IROHA_MODEL"] {
        zenz = ZenzEngine(modelPath: path)
    } else {
        zenz = ZenzEngine()
    }
    let store: UserDictionaryStore
    if let path = ProcessInfo.processInfo.environment["IROHA_USER_DICT"] {
        store = UserDictionaryStore(url: URL(fileURLWithPath: path))
    } else {
        store = .shared
    }
    let learning: LearningStore
    if let path = ProcessInfo.processInfo.environment["IROHA_LEARNING"] {
        learning = LearningStore(url: URL(fileURLWithPath: path))
    } else {
        learning = .shared
    }
    return LearningEngine(
        base: UserDictionaryEngine(base: zenz, dictionary: { store.current }),
        dictionary: { learning.current })
}

func convertAndPrint(engine: any ConversionEngine, reading: String, context: String, count: Int = 1) async {
    let kana = romajiToKana(reading)
    do {
        let start = ContinuousClock.now
        let candidates = try await engine.convert(reading: kana, context: context, candidateCount: count)
        let elapsed = start.duration(to: .now)
        let ms = Double(elapsed.components.attoseconds) / 1e15 + Double(elapsed.components.seconds) * 1e3
        print("\(kana) -> \(candidates.joined(separator: " / "))  [\(String(format: "%.1f", ms))ms]")
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
    let pairs: [(reading: String, expected: String)] = content
        .split(separator: "\n")
        .compactMap { line in
            let parts = line.split(separator: "\t")
            guard parts.count >= 2 else { return nil }
            return (String(parts[0]), String(parts[1]))
        }
    let engine = makeEngine()
    // ウォームアップ（モデルロードを計測から除外）
    _ = try? await engine.convert(reading: "うぉーむあっぷ", context: "", candidateCount: 1)

    var exactMatches = 0
    var totalEditDistance = 0
    var totalExpectedLength = 0
    var totalMilliseconds = 0.0
    for (reading, expected) in pairs {
        let start = ContinuousClock.now
        let result = (try? await engine.convert(reading: reading, context: "", candidateCount: 1).first) ?? reading
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
    let engine = makeEngine()
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
