import Foundation
import IrohaCore
import IrohaDocuments

// iroha-cli docs …: 自分の文書（PDF・Word など）から語を集める機能の計測（段階 0。2026-11-07）。
//
//   iroha-cli docs terms [フォルダ] [--min-count N] [--limit N] [--json 出力.json]
//       文書から集めた語の一覧（出現回数の多い順）。フォルダの既定は <データフォルダ>/documents
//   iroha-cli docs notation [フォルダ] [--limit N]
//       表記の好み: 読みごとに、表記の違い（及び／および など）を何回ずつ使ったか
//   iroha-cli docs evalset [フォルダ] --out 出力.jsonl [--min-length 8] [--max-length 80]
//       文書の文から評価用の項目（読み・正解・直前の文）を作る。読みは分かち書きの自動推定なので、
//       人が確かめて直してから使う（固有名詞の読みを間違えることがある）
//   iroha-cli docs bench <評価.jsonl> [フォルダ] [--context none|previous] [--examples N] [--dump 出力.jsonl]
//       評価項目を今のエンジン（IME と同じ構成・ユーザ辞書と学習は空）で変換し、
//       ① 文の完全一致・CER、② 間違えた文のうち「ほかの文書に出てくる語」を出せていない文の数、
//       ③ その語を単独で変換したときの第一候補・候補 10 件の中の順位、
//       ④ 変換結果を表記の好みで書き換えたときに直る文・壊れる文の数（占める割合の下限ごと）を出す。
//       語の一覧は試す文の文書を除いて作る（leave-one-document-out。その文書を書く前の状態に近づける）

func runDocumentsCommand(_ arguments: [String]) async {
    guard let subcommand = arguments.first else {
        printDocumentsUsage()
        exit(1)
    }
    let rest = Array(arguments.dropFirst())
    switch subcommand {
    case "terms": documentTerms(rest)
    case "notation": documentNotation(rest)
    case "evalset": documentEvalset(rest)
    case "bench": await documentBench(rest)
    default:
        printDocumentsUsage()
        exit(1)
    }
}

private func printDocumentsUsage() {
    FileHandle.standardError.write("""
    使い方:
      iroha-cli docs terms [フォルダ] [--min-count N] [--limit N] [--json 出力.json]
      iroha-cli docs notation [フォルダ] [--limit N]
      iroha-cli docs evalset [フォルダ] --out 出力.jsonl [--min-length 8] [--max-length 80]
      iroha-cli docs bench <評価.jsonl> [フォルダ] [--context none|previous] [--examples N] [--dump 出力.jsonl]

    """.data(using: .utf8)!)
}

/// 位置引数とオプション（--名前 値）に分ける
private func parseOptions(_ arguments: [String]) -> (positional: [String], options: [String: String]) {
    var positional: [String] = []
    var options: [String: String] = [:]
    var index = 0
    while index < arguments.count {
        let argument = arguments[index]
        if argument.hasPrefix("--"), index + 1 < arguments.count {
            options[String(argument.dropFirst(2))] = arguments[index + 1]
            index += 2
        } else {
            positional.append(argument)
            index += 1
        }
    }
    return (positional, options)
}

private func loadCollection(_ path: String?) -> DocumentCollection {
    let folder = path.map { URL(fileURLWithPath: $0, isDirectory: true) } ?? DocumentCollection.defaultFolderURL
    let start = ContinuousClock.now
    let collection = DocumentCollection(folder: folder)
    let seconds = Double(start.duration(to: .now).components.attoseconds) / 1e18
        + Double(start.duration(to: .now).components.seconds)
    stderrLine("文書: \(collection.documents.count) 件（\(folder.path)、\(String(format: "%.1f", seconds)) 秒）")
    for failure in collection.failures {
        stderrLine("  読めなかった: \(failure.name)（\(failure.reason)）")
    }
    if collection.documents.isEmpty {
        stderrLine("対応する文書がありません（\(DocumentTextReader.supportedExtensions.sorted().joined(separator: " "))）")
        exit(1)
    }
    return collection
}

private func stderrLine(_ text: String) {
    FileHandle.standardError.write((text + "\n").data(using: .utf8)!)
}

// MARK: - terms

private func documentTerms(_ arguments: [String]) {
    let (positional, options) = parseOptions(arguments)
    let collection = loadCollection(positional.first)
    let minCount = Int(options["min-count"] ?? "") ?? 2
    let limit = Int(options["limit"] ?? "") ?? 100
    let vocabulary = collection.vocabulary
    let shown = vocabulary.terms.filter { $0.count >= minCount }
    for document in collection.documents {
        print("  \(document.name): 文 \(document.sentences.count)・語の出現 \(document.terms.count)")
    }
    print("語（表記と読みの組）: \(vocabulary.terms.count) 種類、うち \(minCount) 回以上: \(shown.count) 種類")
    for term in shown.prefix(limit) {
        print("\(term.count)\t\(term.documents.count)文書\t\(term.surface)\t\(term.reading)")
    }
    if let path = options["json"] {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        do {
            try encoder.encode(vocabulary).write(to: URL(fileURLWithPath: path))
            stderrLine("書き出し: \(path)")
        } catch {
            stderrLine("書き出せません: \(path)（\(error)）")
            exit(1)
        }
    }
}

// MARK: - notation

private func documentNotation(_ arguments: [String]) {
    let (positional, options) = parseOptions(arguments)
    let collection = loadCollection(positional.first)
    let limit = Int(options["limit"] ?? "") ?? 100
    let groups = collection.notation.variantGroups()
    print("表記の違いがある読み（合計 2 回以上）: \(groups.count) 個")
    for group in groups.prefix(limit) {
        print("\(group.reading)\t" + group.surfaces.map { "\($0.surface) \($0.count)" }.joined(separator: "、"))
    }
}

// MARK: - evalset

/// 評価の 1 項目
struct DocumentEvalItem: Codable {
    let document: String
    /// 文書の中の文の番号（0 から）
    let sentence: Int
    let reading: String
    let expected: String
    /// 同じ文書の直前の文（無ければ空）
    let context: String
}

/// 正解・出力を比べる前にそろえる（論文の「，．」と iroha の「、。」の違いを無視する）
func normalizeDocumentText(_ text: String) -> String {
    text.replacingOccurrences(of: "，", with: "、").replacingOccurrences(of: "．", with: "。")
}

/// 文を評価項目にできるか確かめて、読みと正解を返す。
/// 使うのは、ひらがな・カタカナ・漢字・読点だけでできていて漢字を含み、すべての語に読みが付いた文。
/// 文末の句点・感嘆符は除く（打ち終わってから付けることが多く、比べる意味がない）
func documentEvalPair(_ tokens: [ReadingToken], minLength: Int, maxLength: Int) -> (reading: String, expected: String)? {
    var tokens = tokens
    while let last = tokens.last, ["。", "．", "！", "？", "｡"].contains(last.surface) { tokens.removeLast() }
    var reading = ""
    var expected = ""
    var hasKanji = false
    for token in tokens {
        guard let tokenReading = token.reading else { return nil }
        for scalar in token.surface.unicodeScalars {
            if DocumentText.isKanji(scalar) { hasKanji = true; continue }
            if DocumentText.isHiragana(scalar) || DocumentText.isKatakana(scalar) { continue }
            if scalar == "、" || scalar == "，" { continue }
            return nil
        }
        reading += tokenReading
        expected += token.surface
    }
    expected = normalizeDocumentText(expected)
    guard hasKanji, (minLength...maxLength).contains(expected.count), reading != expected else { return nil }
    return (reading, expected)
}

private func documentEvalset(_ arguments: [String]) {
    let (positional, options) = parseOptions(arguments)
    guard let out = options["out"] else {
        printDocumentsUsage()
        exit(1)
    }
    let minLength = Int(options["min-length"] ?? "") ?? 8
    let maxLength = Int(options["max-length"] ?? "") ?? 80
    let collection = loadCollection(positional.first)
    var items: [DocumentEvalItem] = []
    for document in collection.documents {
        var count = 0
        for (index, tokens) in document.sentences.enumerated() {
            guard let pair = documentEvalPair(tokens, minLength: minLength, maxLength: maxLength) else { continue }
            let context = index > 0
                ? normalizeDocumentText(document.sentences[index - 1].map(\.surface).joined()) : ""
            items.append(DocumentEvalItem(
                document: document.name, sentence: index, reading: pair.reading, expected: pair.expected,
                context: context))
            count += 1
        }
        print("  \(document.name): 文 \(document.sentences.count) のうち \(count) を項目にした")
    }
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
    var lines = ""
    for item in items {
        guard let data = try? encoder.encode(item), let line = String(data: data, encoding: .utf8) else { continue }
        lines += line + "\n"
    }
    do {
        try lines.write(toFile: out, atomically: true, encoding: .utf8)
    } catch {
        stderrLine("書き出せません: \(out)（\(error)）")
        exit(1)
    }
    print("項目: \(items.count) 件 → \(out)")
}

// MARK: - bench

private func documentBench(_ arguments: [String]) async {
    let (positional, options) = parseOptions(arguments)
    guard let evalPath = positional.first,
          let content = try? String(contentsOfFile: evalPath, encoding: .utf8) else {
        printDocumentsUsage()
        exit(1)
    }
    let decoder = JSONDecoder()
    let items = content.split(separator: "\n").compactMap {
        try? decoder.decode(DocumentEvalItem.self, from: Data($0.utf8))
    }
    guard !items.isEmpty else {
        stderrLine("評価項目が読めません: \(evalPath)")
        exit(1)
    }
    let usesContext = options["context"] == "previous"
    let exampleCount = Int(options["examples"] ?? "") ?? 30
    let collection = loadCollection(positional.count >= 2 ? positional[1] : nil)

    // 文書ごとに「その文書を除いた語の一覧」（表記 → 回数。読みは評価項目の読みとの照合に使う）
    let full = collection.vocabulary
    var folds: [String: [DocumentVocabulary.Term]] = [:]
    for document in collection.documents {
        folds[document.name] = full.excluding(document: document.name, occurrences: document.terms).terms
            .filter { $0.surface.count >= 2 }
    }

    // 文書ごとに「その文書を除いた表記の好み」
    let fullNotation = collection.notation
    var notationFolds: [String: NotationPreference] = [:]
    for document in collection.documents {
        notationFolds[document.name] = fullNotation.subtracting(document.notation)
    }
    let tokenizer = SystemReadingTokenizer()
    let shares = [0.5, 0.7, 0.9]
    struct NotationTally {
        var correct = 0
        var fixed = 0
        var broken = 0
        var fixedExamples: [String] = []
        var brokenExamples: [String] = []
    }
    var notationTallies = Array(repeating: NotationTally(), count: shares.count)

    let engine = makeEngine(userData: .evaluation)
    _ = try? await engine.convert(reading: "うぉーむあっぷ", context: "", candidateCount: 1)
    stderrLine("左文脈: \(usesContext ? "直前の文" : "なし")（--context previous で直前の文）")

    struct Missed: Hashable {
        let document: String
        let surface: String
        let reading: String
        let count: Int
    }
    var exact = 0
    var totalDistance = 0
    var totalLength = 0
    var totalMilliseconds = 0.0
    // 間違えた文のうち、ほかの文書に出てくる語（回数 1・2・3 以上）を出せていない文の数
    var missedByThreshold = [1: 0, 2: 0, 3: 0]
    var missedTerms: Set<Missed> = []
    var examples: [String] = []
    var dumpLines = ""

    for (number, item) in items.enumerated() {
        let start = ContinuousClock.now
        let output = (try? await engine.convert(
            reading: item.reading, context: usesContext ? item.context : "", candidateCount: 1).first) ?? item.reading
        let elapsed = start.duration(to: .now)
        totalMilliseconds += Double(elapsed.components.attoseconds) / 1e15 + Double(elapsed.components.seconds) * 1e3
        let normalized = normalizeDocumentText(output)
        let distance = editDistance(Array(normalized), Array(item.expected))
        totalDistance += distance
        totalLength += item.expected.count
        let correct = normalized == item.expected
        var missing: [DocumentVocabulary.Term] = []
        if correct {
            exact += 1
        } else {
            missing = (folds[item.document] ?? []).filter {
                item.expected.contains($0.surface) && item.reading.contains($0.reading)
                    && !normalized.contains($0.surface)
            }
            // 長い語に含まれる短い語は数えない（「視覚障害者」を出せていれば「視覚障害」も出せていない）
            missing = missing.filter { term in
                !missing.contains { $0.surface != term.surface && $0.surface.contains(term.surface) }
            }
            for threshold in missedByThreshold.keys where missing.contains(where: { $0.count >= threshold }) {
                missedByThreshold[threshold, default: 0] += 1
            }
            for term in missing {
                missedTerms.insert(Missed(
                    document: item.document, surface: term.surface, reading: term.reading, count: term.count))
            }
            if examples.count < exampleCount {
                var lines = "  ✗ [\(item.document)] \(item.reading)\n      出力: \(normalized)\n      正解: \(item.expected)"
                if !missing.isEmpty {
                    lines += "\n      ほかの文書の語: "
                        + missing.map { "\($0.surface)（\($0.reading)・\($0.count)回）" }.joined(separator: " ")
                }
                examples.append(lines)
            }
        }
        // ④ 表記の好みで書き換える（文書を除いた数え方で）
        let outputTokens = tokenizer.tokens(in: normalized)
        var rewrites: [String: String] = [:]
        for (index, share) in shares.enumerated() {
            let rewritten = notationFolds[item.document]?.rewrite(outputTokens, minimumShare: share) ?? normalized
            rewrites[String(share)] = rewritten
            let rewrittenCorrect = rewritten == item.expected
            if rewrittenCorrect { notationTallies[index].correct += 1 }
            let example = "    [\(item.document)]\n      出力: \(normalized)\n      直後: \(rewritten)\n      正解: \(item.expected)"
            if rewrittenCorrect && !correct {
                notationTallies[index].fixed += 1
                if notationTallies[index].fixedExamples.count < 15 { notationTallies[index].fixedExamples.append(example) }
            } else if !rewrittenCorrect && correct {
                notationTallies[index].broken += 1
                if notationTallies[index].brokenExamples.count < 15 { notationTallies[index].brokenExamples.append(example) }
            }
        }
        if options["dump"] != nil {
            let record: [String: Any] = [
                "document": item.document, "sentence": item.sentence, "reading": item.reading,
                "expected": item.expected, "output": normalized, "correct": correct,
                "missing_terms": missing.map { ["surface": $0.surface, "reading": $0.reading, "count": $0.count] },
                "notation_rewrites": rewrites,
            ]
            if let data = try? JSONSerialization.data(withJSONObject: record, options: [.sortedKeys]),
               let line = String(data: data, encoding: .utf8) {
                dumpLines += line + "\n"
            }
        }
        if (number + 1) % 100 == 0 { stderrLine("  \(number + 1) / \(items.count)") }
    }

    for example in examples { print(example) }
    let wrong = items.count - exact
    print("")
    print("■ 文の変換（ライブ変換と同じ第一候補）")
    print(String(format: "件数: %d  完全一致: %d (%.1f%%)  CER: %.2f%%  平均: %.1fms/変換",
                 items.count, exact, Double(exact) / Double(items.count) * 100,
                 Double(totalDistance) / Double(max(totalLength, 1)) * 100,
                 totalMilliseconds / Double(items.count)))
    print("間違えた文 \(wrong) 件のうち、ほかの文書に出てくる語を出せていない文:")
    for threshold in missedByThreshold.keys.sorted() {
        let count = missedByThreshold[threshold] ?? 0
        print(String(format: "  語の出現 %d 回以上: %d 件（間違えた文の %.1f%%）",
                     threshold, count, Double(count) / Double(max(wrong, 1)) * 100))
    }

    // ③ 出せていなかった語を単独で変換する（候補ウィンドウで選べる位置にあるか）
    let terms = missedTerms.sorted { ($0.document, $0.surface) < ($1.document, $1.surface) }
    var firstHits = 0
    var inTop10 = 0
    var rankLines: [String] = []
    for term in terms {
        let firstCandidate = (try? await engine.convert(reading: term.reading, context: "", candidateCount: 1).first) ?? ""
        let candidates = (try? await engine.convert(reading: term.reading, context: "", candidateCount: 10)) ?? []
        let rank = candidates.prefix(10).firstIndex(of: term.surface).map { $0 + 1 }
        if firstCandidate == term.surface { firstHits += 1 }
        if rank != nil { inTop10 += 1 }
        rankLines.append("\(term.surface)\t\(term.reading)\t\(term.count)回\t第一候補: \(firstCandidate)\t順位: \(rank.map(String.init) ?? "10位外")")
    }
    print("")
    print("■ 出せていなかった語を単独で変換（\(terms.count) 語。文書ごとに数える）")
    if !terms.isEmpty {
        print(String(format: "  第一候補になる: %d 語（%.1f%%）  候補 10 件に入る: %d 語（%.1f%%）  入らない: %d 語",
                     firstHits, Double(firstHits) / Double(terms.count) * 100,
                     inTop10, Double(inTop10) / Double(terms.count) * 100, terms.count - inTop10))
        for line in rankLines.prefix(exampleCount) { print("  \(line)") }
    }

    print("")
    print("■ 変換結果を表記の好みで書き換えた場合（ほかの文書で数えた回数。最も多い表記が 2 回以上・下の割合以上のとき替える）")
    for (index, share) in shares.enumerated() {
        let tally = notationTallies[index]
        print(String(format: "  割合 %.0f%%以上: 完全一致 %d (%.1f%%)  直した %d  壊した %d",
                     share * 100, tally.correct, Double(tally.correct) / Double(items.count) * 100,
                     tally.fixed, tally.broken))
    }
    if let tally = notationTallies.first {
        print("  直した例（割合 \(Int(shares[0] * 100))%以上）:")
        for example in tally.fixedExamples.prefix(exampleCount) { print(example) }
        print("  壊した例（割合 \(Int(shares[0] * 100))%以上）:")
        for example in tally.brokenExamples.prefix(exampleCount) { print(example) }
    }

    if let path = options["dump"] {
        do {
            try dumpLines.write(toFile: path, atomically: true, encoding: .utf8)
            stderrLine("書き出し: \(path)")
        } catch {
            stderrLine("書き出せません: \(path)（\(error)）")
        }
    }
}
