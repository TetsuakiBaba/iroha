import Foundation

/// 長い読みを区切って順に変換する、変換エンジンのデコレータ。
///
/// zenz-v3はおおむね80文字を超える読みで途中や末尾を飛ばし始める（学習データの読みは
/// 90%が53文字以下）。読み制約（`ReadingConstraint`）で終端を止めても、モデル自身が
/// 読みを追えなくなると意味のない文字列を出すだけなので、モデルに渡す読みの長さを
/// `maxChunkLength` 以下に保つのが唯一の対策になる。azooKeyのzenzaiは読みを区切らないが、
/// 辞書ラティスで出力を縛っているので長い読みでも欠けない（irohaのNN経路にはその縛りがない）。
///
/// 長さは80文字（2026-09-29、AJIMEEの問題をつないだ51〜200文字の読み 各25件で実測）。
/// 区切らないと zenz-v3.1-small は81〜120文字で半数、自作T5は121〜160文字で半数の出力が
/// 1割以上欠けた。一方で51〜80文字は区切らないほうが正確だった（区切りの前後で文脈が途切れる）
/// ため、50文字から延ばした。ただし区切りの中は打鍵ごとに丸ごと変換し直すので、
/// 1打鍵あたりの時間は区切りの長さに比例して伸びる。
///
/// 区切り方（先頭から順に決める）:
/// 1. 先頭 `maxChunkLength` 文字の窓の中に句読点があれば、その直後で切る
/// 2. なければ窓をいったん変換し、`ReadingAligner` の文節境界のうち最後の文節を除いた
///    位置で切る（窓の末尾は語の途中で切れている可能性があるため）。この変換結果は
///    その区切りの結果としてそのまま使う
/// 3. 境界が見つからなければ窓の長さで切る
///
/// 各区切りは直前までの変換結果を左文脈にして変換する。区切りの変換結果は
/// （読み, 文脈）をキーにキャッシュするので、ライブ変換で末尾に1文字ずつ足していく間、
/// 打鍵ごとに実際に再変換されるのは末尾の区切りだけになる。
///
/// `maxChunkLength` 以下の読みはそのまま素通しする（既定のふるまいは変わらない）。
public actor ChunkedConversionEngine: ConversionEngine {

    /// モデルに渡す読みの最大文字数。zenz-v3.1-small が区切らずに読みを追えた長さの上限（上記の実測）
    public static let defaultMaxChunkLength = 80

    private let base: any ConversionEngine
    private let maxChunkLength: Int
    /// 区切りをこれより短くはしない（句読点や文節境界が窓の先頭付近にしかないとき）
    private let minChunkLength: Int

    /// 区切りの変換結果のキャッシュ（読み + 文脈 → 変換結果）。古いものから捨てる
    private var cache: [CacheKey: String] = [:]
    private var cacheOrder: [CacheKey] = []
    private let cacheLimit = 64

    private struct CacheKey: Hashable {
        var reading: String
        var context: String
    }

    public init(base: any ConversionEngine, maxChunkLength: Int = ChunkedConversionEngine.defaultMaxChunkLength) {
        precondition(maxChunkLength > 0)
        self.base = base
        self.maxChunkLength = maxChunkLength
        self.minChunkLength = max(1, maxChunkLength / 3)
    }

    public func prewarm() async throws {
        try await base.prewarm()
    }

    public func convert(reading: String, context: String, candidateCount: Int) async throws -> [String] {
        let characters = Array(reading)
        guard characters.count > maxChunkLength else {
            return try await base.convert(reading: reading, context: context, candidateCount: candidateCount)
        }

        // 長い読みは候補を並べる場面（文節の候補ウィンドウ）には来ないので、1候補だけ返す
        var result = ""
        var context = context
        var index = 0
        while index < characters.count {
            try Task.checkCancellation()
            let remaining = characters.count - index
            if remaining <= maxChunkLength {
                result += try await convertCached(String(characters[index...]), context: context)
                break
            }
            let window = String(characters[index..<(index + maxChunkLength)])
            let (cut, converted) = try await cutWindow(window, context: context)
            result += converted
            context += converted
            index += cut
        }
        return [result]
    }

    /// 窓（maxChunkLength文字）をどこで切るかと、その区切りの変換結果を返す
    private func cutWindow(_ window: String, context: String) async throws -> (cut: Int, converted: String) {
        // 1. 句読点の直後で切る（次の区切りを文の頭から始められる）
        if let cut = Self.punctuationCut(window, minimum: minChunkLength) {
            let chunk = String(window.prefix(cut))
            return (cut, try await convertCached(chunk, context: context))
        }

        // 2. 窓を変換して文節境界で切る。最後の文節は語の途中で切れているかもしれないので次へ回す
        let converted = try await convertCached(window, context: context)
        let segments = ReadingAligner.segmentReading(window, conversion: converted)
        if segments.count >= 2 {
            let head = segments.dropLast()
            let cut = head.reduce(0) { $0 + $1.reading.count }
            if cut >= minChunkLength {
                return (cut, head.map(\.conversion).joined())
            }
        }

        // 3. 境界が取れなければ窓ごと使う
        return (window.count, converted)
    }

    /// 窓の中で最後に現れる句読点の直後の位置（minimum以上のもの）。なければnil
    static func punctuationCut(_ window: String, minimum: Int) -> Int? {
        let characters = Array(window)
        var index = characters.count - 1
        while index >= 0 {
            if "、。！？，．".contains(characters[index]) {
                let cut = index + 1
                return cut >= minimum ? cut : nil
            }
            index -= 1
        }
        return nil
    }

    private func convertCached(_ reading: String, context: String) async throws -> String {
        let key = CacheKey(reading: reading, context: context)
        if let cached = cache[key] {
            InferenceTimer.current?.note(.cache)
            return cached
        }
        let converted = try await base.convert(reading: reading, context: context, candidateCount: 1).first ?? reading
        if cache[key] == nil {
            cacheOrder.append(key)
            if cacheOrder.count > cacheLimit {
                cache.removeValue(forKey: cacheOrder.removeFirst())
            }
        }
        cache[key] = converted
        return converted
    }
}
