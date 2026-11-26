import Foundation
import IrohaCore
import IrohaDocuments

// 学習データの各行の変換結果に macOS の分かち書き（SystemReadingTokenizer）で読みを付け直し、JSONL に出す。
// データの読みとの突き合わせは experiments/train-data-audit/reading_check.py で行う。
//
//   iroha-cli train-reading <train.txt> <out.jsonl> [--shard 番号/数]
//
// tokens は [表記, 読み（ひらがな。付かなければ null）] の並び。line は入力の行番号（0 始まり）。
// 入力は 1 行ずつ読む（train-full.txt は 26GB あり、丸ごとは読めない）。
// --shard 2/8 は行番号を 8 で割った余りが 2 の行だけを処理する（複数のプロセスで分けて走らせるとき）
func runTrainReadingCommand(_ arguments: [String]) {
    guard arguments.count >= 2, let input = fopen(arguments[0], "r") else {
        FileHandle.standardError.write("使い方: iroha-cli train-reading <train.txt> <out.jsonl> [--shard 番号/数]\n".data(using: .utf8)!)
        exit(1)
    }
    defer { fclose(input) }
    var shardIndex = 0, shardCount = 1
    if let index = arguments.firstIndex(of: "--shard"), index + 1 < arguments.count {
        let parts = arguments[index + 1].split(separator: "/").compactMap { Int($0) }
        if parts.count == 2, parts[1] > 0, (0..<parts[1]).contains(parts[0]) {
            shardIndex = parts[0]
            shardCount = parts[1]
        }
    }
    FileManager.default.createFile(atPath: arguments[1], contents: nil)
    guard let out = FileHandle(forWritingAtPath: arguments[1]) else { exit(1) }
    defer { try? out.close() }
    struct Row: Encodable {
        let line: Int
        let reading: String
        let output: String
        let tokens: [[String?]]
    }
    let tokenizer = SystemReadingTokenizer()
    let encoder = JSONEncoder()
    var buffer = Data()
    var linePointer: UnsafeMutablePointer<CChar>?
    var capacity = 0
    defer { free(linePointer) }
    var lineIndex = -1
    while getline(&linePointer, &capacity, input) > 0 {
        lineIndex += 1
        guard lineIndex % shardCount == shardIndex, let linePointer else { continue }
        var line = String(cString: linePointer)
        if line.hasSuffix("\n") { line.removeLast() }
        guard let inputRange = line.range(of: "\u{EE00}"),
              let outputRange = line.range(of: "\u{EE01}", range: inputRange.upperBound..<line.endIndex) else { continue }
        let reading = String(line[inputRange.upperBound..<outputRange.lowerBound])
        let output = String(line[outputRange.upperBound...])
        let tokens = tokenizer.tokens(in: output).map { [$0.surface, $0.reading] }
        guard let data = try? encoder.encode(Row(line: lineIndex, reading: reading, output: output, tokens: tokens)) else { continue }
        buffer.append(data)
        buffer.append(0x0A)
        if buffer.count >= 1 << 20 {
            out.write(buffer)
            buffer.removeAll(keepingCapacity: true)
        }
    }
    out.write(buffer)
}

// 学習データ（U+EE02文脈 U+EE00カタカナ読み U+EE01変換結果 の行）の各行を、かな漢字変換モデルで採点する。
// 学習データの中の「読みと変換結果の対応が誤っている行」を拾う調査用（experiments/train-data-audit/）。
//
//   iroha-cli train-score <train.txt> <out.jsonl> [--limit 件数]
//
// 1 行ごとに次を JSONL に出す:
//   target    データの変換結果の log P(変換結果 + 終端 | 文脈, 読み)（teacher forcing。読み制約なし）
//   greedy    モデルが貪欲生成した出力（読み制約あり。IME と同じ生成）
//   greedyLP  その出力を target と同じ関数で採点した値
//   margin    greedyLP − target（モデルが自分の出力をデータの答えよりどれだけ高く見るか。0 なら一致）
// モデルは IROHA_MODEL で指定する。
func runTrainScoreCommand(_ arguments: [String]) async {
    guard arguments.count >= 2 else {
        FileHandle.standardError.write("使い方: iroha-cli train-score <train.txt> <out.jsonl> [--limit 件数]\n".data(using: .utf8)!)
        exit(1)
    }
    var limit = Int.max
    if let index = arguments.firstIndex(of: "--limit"), index + 1 < arguments.count {
        limit = Int(arguments[index + 1]) ?? limit
    }
    guard let text = try? String(contentsOfFile: arguments[0], encoding: .utf8) else {
        FileHandle.standardError.write("読めません: \(arguments[0])\n".data(using: .utf8)!)
        exit(1)
    }
    FileManager.default.createFile(atPath: arguments[1], contents: nil)
    guard let out = FileHandle(forWritingAtPath: arguments[1]) else {
        FileHandle.standardError.write("書けません: \(arguments[1])\n".data(using: .utf8)!)
        exit(1)
    }
    defer { try? out.close() }

    struct Row: Encodable {
        let line: Int
        let context: String
        let reading: String
        let output: String
        let greedy: String
        let target: Float
        let greedyLP: Float
        let margin: Float
    }

    let zenz = makeZenz()
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.withoutEscapingSlashes]
    let started = Date()
    var processed = 0
    for (lineIndex, rawLine) in text.split(separator: "\n", omittingEmptySubsequences: false).enumerated() {
        if processed >= limit { break }
        let line = String(rawLine)
        guard let inputRange = line.range(of: "\u{EE00}"),
              let outputRange = line.range(of: "\u{EE01}", range: inputRange.upperBound..<line.endIndex) else { continue }
        var context = String(line[line.startIndex..<inputRange.lowerBound])
        if context.hasPrefix("\u{EE02}") { context.removeFirst() }
        let readingKatakana = String(line[inputRange.upperBound..<outputRange.lowerBound])
        let output = String(line[outputRange.upperBound...])
        guard !readingKatakana.isEmpty, !output.isEmpty else { continue }
        let reading = katakanaToHiragana(readingKatakana)
        do {
            let greedy = try await zenz.convertWithConfidence(reading: reading, context: context).text
            let candidates = greedy == output ? [output] : [output, greedy]
            let scores = try await zenz.score(candidates: candidates, reading: reading, context: context)
            let target = scores[0]
            let greedyLP = scores.count > 1 ? scores[1] : scores[0]
            let row = Row(line: lineIndex, context: context, reading: readingKatakana, output: output,
                          greedy: greedy, target: target, greedyLP: greedyLP, margin: greedyLP - target)
            var data = try encoder.encode(row)
            data.append(0x0A)
            out.write(data)
        } catch {
            FileHandle.standardError.write("行 \(lineIndex): \(error.localizedDescription)\n".data(using: .utf8)!)
        }
        processed += 1
        if processed % 500 == 0 {
            let elapsed = Date().timeIntervalSince(started)
            FileHandle.standardError.write(String(format: "%d 行 %.1f 秒（%.1f ms/行）\n",
                                                  processed, elapsed, elapsed * 1000 / Double(processed)).data(using: .utf8)!)
        }
    }
    let elapsed = Date().timeIntervalSince(started)
    FileHandle.standardError.write(String(format: "完了: %d 行 %.1f 秒（%.1f ms/行）\n",
                                          processed, elapsed, elapsed * 1000 / Double(max(processed, 1))).data(using: .utf8)!)
}
