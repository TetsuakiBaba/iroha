import Foundation

/// 本体を指で叩いたタップを、加速度センサの揺れと入力（キー・修飾キー・クリック）の時刻から数える（タップ入力。2026-10-24）。
///
/// 規則は実験の道具（`experiments/keystroke-force/` の `TapDetector`）と同じで、指示つきの記録 160 回で
/// 回数の正解 148 回（92.5%）だった。揺れの山のうち、前後 `causeWindow` 以内に入力があるものはその入力の揺れ、
/// 直前 `echoWindow` 以内の山より十分小さいものは残響として数えず、残りをタップとする。
/// センサの値を読むのは iroha 側（`KeystrokeAccelerometer`・`TapInput`）で、ここは時刻と値だけを受け取る。
/// 時刻は秒で、揺れと入力は同じ時計で渡す。1 つのスレッドから使う
public struct TapGestureDetector {
    public struct Settings: Sendable {
        /// これ未満の山は数えない（mg）
        public var absoluteThreshold: Float = 10
        /// 雑音の何倍を超えたら山とするか
        public var noiseFactor: Float = 5
        /// 雑音は山の `noiseWindow` 秒前から 30ms 前までの揺れの、小さいほうから `noiseQuantile` の値。
        /// 直前 0.3 秒の中央値だと、間隔の短い 2 回目のタップで 1 回目の揺れが雑音に入って閾値が上がった
        public var noiseWindow = 1.0
        public var noiseQuantile: Float = 0.25
        /// 山のあと次の山を数えない時間（秒）。強く叩くと約 80ms 後に跳ね返りの山が出る
        public var refractory = 0.12
        /// 次の山を数えるには、揺れがいったん閾値のこの割合まで下がる必要がある
        public var rearmRatio: Float = 0.5
        /// 山の始まりからこの時間（秒）以内に入力があれば、その入力の揺れとみなす
        public var causeWindow = 0.05
        /// 直前 `echoWindow` 秒の山の最大のこの割合に届かない山は残響とする。テーブルを叩くと本物の約 130〜175ms 後に出た
        public var echoRatio: Float = 0.5
        public var echoWindow = 0.25
        /// 同じまとまりとみなすタップの間隔の上限（秒）
        public var maxGap = 0.45

        public init() {}
    }

    public enum ImpactKind: Equatable, Sendable {
        case tap
        /// 入力（キー・修飾キー・クリック）の揺れ
        case input
        case echo
    }

    public struct Impact: Equatable, Sendable {
        /// 閾値を超えた時刻
        public var onset: Double
        public var peak: Float
        public var kind: ImpactKind
    }

    public let settings: Settings
    /// 判定を終えた山（古いものは捨てる）
    public private(set) var impacts: [Impact] = []
    /// この時刻までに始まった山は判定を終えている
    public private(set) var processedUntil = -Double.infinity

    /// 山の形（30ms）と、山のあとの入力（`causeWindow`）が届くのを待ってから判定する
    private var lookahead: Double { max(0.15, settings.causeWindow + 0.05) }
    private var times: [Double] = []
    private var magnitudes: [Float] = []
    private var cursor = 0
    private var inputs: [Double] = []
    private var armed = true
    private var armTime = -Double.infinity
    private var rearmLevel: Float = 0
    private var noiseCache: (time: Double, value: Float)?

    public init(settings: Settings = Settings()) {
        self.settings = settings
    }

    /// 揺れ（重力・姿勢を引いた大きさ、mg）を 1 件足す。時刻は前の値より後であること
    public mutating func addSample(t: Double, magnitude: Float) {
        times.append(t)
        magnitudes.append(magnitude)
        process(upTo: t - lookahead)
        trim(now: t)
    }

    /// 入力の時刻を足す（届く順は前後してよい）
    public mutating func addInput(t: Double) {
        inputs.append(t)
    }

    /// `start...end` に始まったタップの数。タップの間隔がどこかで `maxGap` を超えるなら nil（ひとまとまりではない）。
    /// `processedUntil >= end` になってから呼ぶ
    public func tapCount(from start: Double, to end: Double) -> Int? {
        let onsets = impacts.filter { $0.kind == .tap && (start...end).contains($0.onset) }.map(\.onset)
        for (a, b) in zip(onsets, onsets.dropFirst()) where b - a > settings.maxGap { return nil }
        return onsets.count
    }

    private mutating func process(upTo limit: Double) {
        while cursor < times.count, times[cursor] <= limit {
            let t = times[cursor], m = magnitudes[cursor]
            if !armed {
                if t >= armTime && m < rearmLevel { armed = true }
            } else if m >= settings.absoluteThreshold {
                let threshold = max(settings.absoluteThreshold, settings.noiseFactor * noise(at: t))
                if m >= threshold {
                    record(onset: t)
                    armed = false
                    armTime = t + settings.refractory
                    rearmLevel = threshold * settings.rearmRatio
                }
            }
            processedUntil = t
            cursor += 1
        }
    }

    private mutating func record(onset: Double) {
        var peak: Float = 0
        var i = cursor
        while i < times.count, times[i] < onset + 0.03 {
            peak = max(peak, magnitudes[i])
            i += 1
        }
        let caused = inputs.contains { abs($0 - onset) <= settings.causeWindow }
        let recent = impacts.reversed().prefix { $0.onset >= onset - settings.echoWindow }.map(\.peak).max() ?? 0
        let kind: ImpactKind = caused ? .input
            : settings.echoRatio > 0 && peak < recent * settings.echoRatio ? .echo : .tap
        impacts.append(Impact(onset: onset, peak: peak, kind: kind))
    }

    /// 雑音は揺れが閾値を超えるたびに要るが、並べ替えが重いので 20ms ごとに測り直す
    private mutating func noise(at t: Double) -> Float {
        if let cache = noiseCache, t - cache.time < 0.02 { return cache.value }
        var window: [Float] = []
        var i = cursor - 1
        while i >= 0, times[i] >= t - settings.noiseWindow {
            if times[i] < t - 0.03 { window.append(magnitudes[i]) }
            i -= 1
        }
        window.sort()
        let value = window.isEmpty ? 0 : window[min(window.count - 1, Int(Float(window.count) * settings.noiseQuantile))]
        noiseCache = (t, value)
        return value
    }

    /// 雑音の範囲より古い揺れ・使わない山と入力を捨てる
    private mutating func trim(now: Double) {
        let keepFrom = now - lookahead - settings.noiseWindow - 0.1
        if let first = times.first, first < keepFrom - 1 {
            let drop = times.firstIndex { $0 >= keepFrom } ?? times.count
            let n = min(drop, cursor)
            times.removeFirst(n)
            magnitudes.removeFirst(n)
            cursor -= n
            impacts.removeAll { $0.onset < now - 30 }
            inputs.removeAll { $0 < now - 30 }
        }
    }
}
