import Foundation
import IrohaCore
import IrohaVoice

// 「こえ」モードの確認用（音声認識 → 読み → かな漢字変換を、IME を通さずに 1 件ずつ見る）。
//
// 使い方:
//   iroha-cli voice [--recognizer speech|dictation] [--context 文脈] <音声ファイル>...
//       音声ファイル（aiff・wav・m4a など）を認識し、認識結果・読み・iroha の変換を出す。
//       読み上げ音声は `say -v Kyoko -o a.aiff "今日は良い天気です"` で作れる
//   iroha-cli voice --stream [--recognizer …] <音声ファイル>...
//       マイクで録ったときと同じ経路（実時間の速さで少しずつ渡す）で認識し、途中結果が届いた時刻も出す
//   iroha-cli voice --text <表記>... [--context 文脈]
//       認識を省き、表記から読みを求めて変換する（読みの復元だけを確かめる）
//
// 変換は IME と同じ順に行う: 読みの部分は左文脈（--context + それまでの出力）つきで変換し、
// 固定部分（英字・数字・読みを求められなかった語）は認識結果の表記のまま入れる

func runVoiceCommand(_ arguments: [String]) async {
    var kind = VoiceRecognizerKind.speech
    var context = ""
    var texts: [String] = []
    var files: [String] = []
    var streaming = false
    var index = 0
    while index < arguments.count {
        let argument = arguments[index]
        let value = index + 1 < arguments.count ? arguments[index + 1] : nil
        switch argument {
        case "--recognizer":
            guard let value, let parsed = VoiceRecognizerKind(rawValue: value) else {
                fail("--recognizer は speech か dictation")
            }
            kind = parsed
            index += 2
        case "--context":
            guard let value else { fail("--context に文脈がありません") }
            context = value
            index += 2
        case "--stream":
            streaming = true
            index += 1
        case "--text":
            guard let value else { fail("--text に表記がありません") }
            texts.append(value)
            index += 2
        default:
            files.append(argument)
            index += 1
        }
    }
    guard !texts.isEmpty || !files.isEmpty else {
        fail("使い方: iroha-cli voice [--recognizer speech|dictation] [--context 文脈] <音声ファイル>... | --text <表記>")
    }

    let engine = makeEngine()
    for text in texts {
        await printVoiceResult(recognized: text, context: context, engine: engine)
    }
    for file in files {
        print("== \(file)")
        do {
            let start = Date()
            let recognized: String
            if streaming {
                // マイクのときと同じ経路。途中結果が届いた時刻を出す
                recognized = try await VoiceRecognition.transcribeStreaming(
                    file: URL(fileURLWithPath: file), kind: kind
                ) { partial in
                    print(String(format: "  途中 %.2f秒 %@", Date().timeIntervalSince(start), partial))
                }
            } else {
                recognized = try await VoiceRecognition.transcribe(file: URL(fileURLWithPath: file), kind: kind)
            }
            print(String(format: "認識（%@・%.0fms）", kind.rawValue, Date().timeIntervalSince(start) * 1000))
            await printVoiceResult(recognized: recognized, context: context, engine: engine)
        } catch {
            FileHandle.standardError.write("認識できません: \(error)\n".data(using: .utf8)!)
        }
    }
}

private func printVoiceResult(recognized: String, context: String, engine: any ConversionEngine) async {
    let transcript = VoiceRecognition.transcript(from: recognized)
    print("  認識  : \(recognized)")
    let parts = transcript.parts.map { part -> String in
        switch part {
        case .reading(let reading, _): return reading
        case .fixed(let text): return "[\(text)]"
        }
    }.joined(separator: " ")
    print("  読み  : \(parts)")
    var output = ""
    for part in transcript.parts {
        switch part {
        case .fixed(let text):
            output += text
        case .reading(let reading, _):
            do {
                let converted = try await engine.convert(
                    reading: reading, context: context + output, candidateCount: 1)
                output += converted.first ?? reading
            } catch {
                output += reading
                FileHandle.standardError.write("変換エラー: \(error)\n".data(using: .utf8)!)
            }
        }
    }
    print("  iroha : \(output)")
    print("  一致  : \(output == transcript.surface ? "認識の表記と同じ" : "認識の表記と違う")")
}

private func fail(_ message: String) -> Never {
    FileHandle.standardError.write("\(message)\n".data(using: .utf8)!)
    exit(1)
}
