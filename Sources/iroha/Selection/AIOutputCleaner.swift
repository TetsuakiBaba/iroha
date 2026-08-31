import Foundation

/// 選択テキスト処理の出力の軽い整形。
/// GenGoのResponseCleanerはMarkdownを全部落とすため移植せず、
/// 「モデルがよく付ける余計な包み」だけを安全な範囲で剥がす
/// （thinkブロックはRemoteTranslator.stripThinkingが先に除去している）
enum AIOutputCleaner {

    static func clean(_ text: String) -> String {
        var cleaned = RemoteTranslator.stripThinking(text)
            .trimmingCharacters(in: .whitespacesAndNewlines)

        // 全体が1つのコードフェンスに包まれていたら中身だけにする
        if cleaned.hasPrefix("```"), cleaned.hasSuffix("```"), cleaned.count > 6 {
            var inner = cleaned.dropFirst(3).dropLast(3)
            // 開きフェンス直後の言語名（```text 等）は最初の改行まで捨てる
            if let newline = inner.firstIndex(of: "\n") {
                let firstLine = inner[..<newline]
                if !firstLine.contains(" ") && firstLine.count <= 20 {
                    inner = inner[inner.index(after: newline)...]
                }
            }
            let candidate = inner.trimmingCharacters(in: .whitespacesAndNewlines)
            if !candidate.isEmpty { cleaned = candidate }
        }

        // 全体が1組の引用符・括弧に包まれていたら剥がす（中に同じ記号が無いときだけ）
        let quotePairs: [(Character, Character)] = [
            ("「", "」"), ("『", "』"), ("\u{201C}", "\u{201D}"), ("\"", "\""), ("'", "'"),
        ]
        for (open, close) in quotePairs {
            if cleaned.count >= 2, cleaned.first == open, cleaned.last == close {
                let inner = String(cleaned.dropFirst().dropLast())
                if !inner.contains(open), !inner.contains(close), !inner.isEmpty {
                    cleaned = inner.trimmingCharacters(in: .whitespacesAndNewlines)
                }
                break
            }
        }

        return cleaned
    }
}
