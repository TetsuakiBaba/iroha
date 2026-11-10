import Foundation

/// 音声認識の表記と、同じ発話を読みに戻して iroha で変換した結果の、違う箇所だけを取り出す（「こえ」モード。2026-11-10）。
///
/// 長い発話では全文を並べても見比べられないので、違う箇所を前後 1 文字つきで出す（「何時か → いつか」）。
/// 違う箇所の間に同じ文字が 1 文字しかなければ 1 か所にまとめる
public enum VoiceComparison {

    public struct Difference: Sendable, Equatable {
        public let recognized: String
        public let converted: String

        public init(recognized: String, converted: String) {
            self.recognized = recognized
            self.converted = converted
        }
    }

    public static func differences(recognized: String, converted: String) -> [Difference] {
        let a = Array(recognized), b = Array(converted)
        guard a != b else { return [] }
        var removed = Set<Int>(), inserted = Set<Int>()
        for change in b.difference(from: a) {
            switch change {
            case .remove(let offset, _, _): removed.insert(offset)
            case .insert(let offset, _, _): inserted.insert(offset)
            }
        }
        // 両方を頭からたどる。消えた文字でも足された文字でもないものは、同じ順に対応している
        var hunks: [(a: Range<Int>, b: Range<Int>)] = []
        var i = 0, j = 0
        var start: (a: Int, b: Int)?
        var commonRun = 0
        func close(at endA: Int, _ endB: Int) {
            guard let opened = start else { return }
            hunks.append((opened.a..<endA, opened.b..<endB))
            start = nil
        }
        while i < a.count || j < b.count {
            if i < a.count, removed.contains(i) {
                if start == nil { start = (i, j) }
                commonRun = 0
                i += 1
            } else if j < b.count, inserted.contains(j) {
                if start == nil { start = (i, j) }
                commonRun = 0
                j += 1
            } else {
                // 同じ文字。2 文字続いたら、そこまでの違いを 1 か所として閉じる
                commonRun += 1
                if commonRun == 2 { close(at: i - 1, j - 1) }
                i += 1
                j += 1
            }
        }
        close(at: a.count - commonRun, b.count - commonRun)

        return hunks.map { hunk in
            // 前後 1 文字（同じ文字）を付ける
            let before = hunk.a.lowerBound > 0 ? 1 : 0
            let after = hunk.a.upperBound < a.count ? 1 : 0
            return Difference(
                recognized: String(a[(hunk.a.lowerBound - before)..<(hunk.a.upperBound + after)]),
                converted: String(b[(hunk.b.lowerBound - before)..<(hunk.b.upperBound + after)]))
        }
    }

    /// 小窓に出す 1 行（「音声認識と同じ」か、違う箇所を最大 `limit` 件）
    public static func summary(recognized: String, converted: String, limit: Int = 3) -> String {
        let all = differences(recognized: recognized, converted: converted)
        guard !all.isEmpty else { return "音声認識と同じ" }
        var text = "音声認識 → iroha: "
            + all.prefix(limit).map { "\($0.recognized) → \($0.converted)" }.joined(separator: "　／　")
        if all.count > limit { text += "　ほか \(all.count - limit) か所" }
        return text
    }
}
