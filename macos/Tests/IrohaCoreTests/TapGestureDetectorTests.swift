import Foundation
import Testing
@testable import IrohaCore

/// 800 件/秒の揺れを作る。`bumps` は（始まりの時刻, 山の mg, 減衰の時定数）
private func feed(_ detector: inout TapGestureDetector, until end: Double,
                  bumps: [(t: Double, peak: Float, decay: Double)], noise: Float = 2) {
    var t = 0.0
    var seed: UInt64 = 1
    while t < end {
        seed = seed &* 6364136223846793005 &+ 1442695040888963407
        var m = noise * Float(seed >> 40) / Float(1 << 24)
        for bump in bumps where t >= bump.t {
            let dt = t - bump.t
            // 1ms で立ち上がって指数で減る
            m += bump.peak * Float(min(1, dt / 0.001) * exp(-dt / bump.decay))
        }
        detector.addSample(t: t, magnitude: m)
        t += 1.0 / 800
    }
}

struct TapGestureDetectorTests {
    @Test func countsDoubleTap() {
        var detector = TapGestureDetector()
        feed(&detector, until: 3, bumps: [(1.0, 40, 0.008), (1.22, 38, 0.008)])
        #expect(detector.processedUntil >= 2)
        #expect(detector.tapCount(from: 0.9, to: 2) == 2)
        #expect(detector.tapCount(from: 1.1, to: 2) == 1)
    }

    @Test func ignoresImpactNearInput() {
        var detector = TapGestureDetector()
        detector.addInput(t: 1.01)
        feed(&detector, until: 3, bumps: [(1.0, 40, 0.008), (1.22, 38, 0.008)])
        #expect(detector.tapCount(from: 0.9, to: 2) == 1)
        #expect(detector.impacts.map(\.kind) == [.input, .tap])
    }

    @Test func ignoresEcho() {
        var detector = TapGestureDetector()
        // テーブルを叩いたときのように、本物の 150ms 後に 3 割の山
        feed(&detector, until: 3, bumps: [(1.0, 40, 0.02), (1.15, 12, 0.02), (1.3, 38, 0.02)])
        #expect(detector.impacts.map(\.kind) == [.tap, .echo, .tap])
        #expect(detector.tapCount(from: 0.9, to: 2) == 2)
    }

    @Test func separatedTapsAreNotOneSequence() {
        var detector = TapGestureDetector()
        feed(&detector, until: 3, bumps: [(1.0, 40, 0.008), (1.6, 38, 0.008)])
        #expect(detector.tapCount(from: 0.9, to: 2) == nil)
    }

    @Test func ignoresSmallShakes() {
        var detector = TapGestureDetector()
        feed(&detector, until: 3, bumps: [(1.0, 8, 0.008), (1.22, 9, 0.008)])
        #expect(detector.tapCount(from: 0.9, to: 2) == 0)
    }

    /// 実験の指示つきの記録（リポジトリ外の `experiments/keystroke-force/recordings/*-guide`）で、
    /// 実験の道具と同じ正解数になるか。記録がなければ何もしない
    @Test func matchesRecordedTrials() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .appendingPathComponent("../../../experiments/keystroke-force/recordings").standardized
        let expected = ["20261024-081010-guide": 75, "20261024-082601-guide": 73]
        for (name, correct) in expected {
            let dir = root.appendingPathComponent(name)
            guard let accel = try? String(contentsOf: dir.appendingPathComponent("accel.csv"), encoding: .utf8),
                  let events = try? String(contentsOf: dir.appendingPathComponent("events.csv"), encoding: .utf8),
                  let trials = try? String(contentsOf: dir.appendingPathComponent("trials.csv"), encoding: .utf8)
            else { continue }
            var detector = TapGestureDetector()
            for line in events.split(separator: "\n").dropFirst() {
                if let t = Double(line.split(separator: ",")[0]) { detector.addInput(t: t) }
            }
            // 実際の使い方と同じく、⌘ を離した時刻まで判定が進んだところで数える（古い山は捨てられるため）
            var pending: [(count: Int, start: Double, end: Double)] = []
            for line in trials.split(separator: "\n", omittingEmptySubsequences: false).dropFirst() where !line.isEmpty {
                let f = line.split(separator: ",", omittingEmptySubsequences: false)
                if let count = Int(f[2]), let start = Double(f[3]), let end = Double(f[4]) { pending.append((count, start, end)) }
            }
            var hits = 0
            for line in accel.split(separator: "\n").dropFirst() {
                let f = line.split(separator: ",")
                guard f.count >= 5, let t = Double(f[0]), let m = Float(f[4]) else { continue }
                detector.addSample(t: t, magnitude: m)
                while let trial = pending.first, detector.processedUntil >= trial.end {
                    if detector.tapCount(from: trial.start, to: trial.end) == trial.count { hits += 1 }
                    pending.removeFirst()
                }
            }
            #expect(hits == correct, "\(name): \(hits)")
        }
    }
}
