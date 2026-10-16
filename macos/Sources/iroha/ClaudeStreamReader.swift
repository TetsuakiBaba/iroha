import Foundation

/// Claude の Messages API のストリーム（SSE）を1行ずつ読み、本文と終わりの理由を集める。
///
/// 通信から切り離してあるのは、決まった形の行を流し込んで読み方を確かめられるようにするため
/// （`RemoteTranslator.streamClaude` が使う）。Foundation だけに依存する
struct ClaudeStreamReader {

    enum Failure: Error, Equatable {
        /// ストリームの途中で届いたエラー（過負荷など）
        case server(String)
        /// Claude が依頼を断った（HTTP 200 で `stop_reason: "refusal"`）。カテゴリは無いこともある
        case refusal(category: String?)

        var message: String {
            switch self {
            case .server(let message): return message
            case .refusal(let category):
                return "Claude が応答を断りました" + (category.map { "（\($0)）" } ?? "")
            }
        }
    }

    /// ここまでに届いた本文（`text_delta` を足したもの）。思考（thinking）は別のブロックで来るので入れない
    private(set) var text = ""
    private(set) var stopReason: String?
    private(set) var refusalCategory: String?
    /// `message_stop` まで読んだ
    private(set) var isFinished = false

    /// 1行を読む。本文が増えたら true。エラーの行なら投げる
    mutating func consume(_ line: String) throws -> Bool {
        guard line.hasPrefix("data:") else { return false }
        let payload = line.dropFirst(5).trimmingCharacters(in: .whitespaces)
        guard let data = payload.data(using: .utf8),
              let event = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let type = event["type"] as? String else { return false }
        switch type {
        case "content_block_delta":
            guard let delta = event["delta"] as? [String: Any], delta["type"] as? String == "text_delta",
                  let piece = delta["text"] as? String, !piece.isEmpty else { return false }
            text += piece
            return true
        case "message_delta":
            let delta = event["delta"] as? [String: Any]
            if let reason = delta?["stop_reason"] as? String { stopReason = reason }
            let details = (delta?["stop_details"] ?? event["stop_details"]) as? [String: Any]
            if let category = details?["category"] as? String { refusalCategory = category }
        case "error":
            let error = event["error"] as? [String: Any]
            throw Failure.server(error?["message"] as? String ?? "Claude API のエラー")
        case "message_stop":
            isFinished = true
        default:
            break
        }
        return false
    }

    /// 読み終えたあとの結果。断られていたら途中までの本文は使わずに投げる
    func result() throws -> String {
        if stopReason == "refusal" { throw Failure.refusal(category: refusalCategory) }
        return text.trimmingCharacters(in: .whitespacesAndNewlines)
    }
}
