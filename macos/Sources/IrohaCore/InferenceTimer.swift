import Foundation

/// 1回の変換要求（または打ち間違いの訂正 1 回）の中で、ニューラルネット（NN）の推論にかかった
/// 時間を集める。設定 > 情報 の「デバッグ表示」で、推論のたびにカーソルの近くへ出すためのもの。
///
/// 変換エンジンはデコレータを重ねた形（学習 → ユーザ辞書 → 長い読みの区切り → 異体字 →
/// 辞書ラティス → zenz）なので、呼び出し側で測った時間には NN 以外の処理と、先に走っている
/// 推論の待ち時間が混ざる。NN の時間は NN を動かす actor（`ZenzEngine`・`TypoNormalizer`）の
/// 内側でしか測れないため、呼び出し側がこの入れ物をタスクローカル値（`current`）に置き、
/// actor の側が計算の前後で時刻を取って足し込む。
///
/// `current` が nil（デバッグ表示が OFF）なら actor の側は時刻も取らない。
/// タスクローカル値は同じタスクの中の呼び出し（actor をまたいでも）に引き継がれるが、
/// `Task.detached` には引き継がれないので、切り離したタスクの中で改めて置くこと
public final class InferenceTimer: @unchecked Sendable {

    /// 今の変換要求に対応する入れ物。呼び出し側が `$current.withValue(_:operation:)` で置く
    @TaskLocal public static var current: InferenceTimer?

    /// NN を通さずに結果を返した理由（NN の回数が 0 のときの説明に使う）
    public enum Shortcut: String, Sendable, CaseIterable {
        /// 学習（読み全体が過去の修正と一致）
        case learning
        /// ユーザ辞書（読み全体が 1 語と一致）
        case userDictionary
        /// 長い読みの区切りのキャッシュ
        case cache
    }

    public struct Snapshot: Sendable, Equatable {
        /// NN の計算にかかった時間の合計（モデルの読み込みは含まない）
        public var neuralNetwork: Duration = .zero
        /// NN を動かした回数（辞書ラティスでは生成と採点で 2 回、長い読みは区切りの数だけ）
        public var neuralNetworkCalls = 0
        /// モデルの読み込みにかかった時間（最初の 1 回だけ 0 でない）
        public var modelLoad: Duration = .zero
        public var shortcuts: Set<Shortcut> = []

        public init() {}
    }

    private let lock = NSLock()
    private var state = Snapshot()

    public init() {}

    public var snapshot: Snapshot {
        lock.lock()
        defer { lock.unlock() }
        return state
    }

    public func addNeuralNetwork(_ duration: Duration) {
        lock.lock()
        state.neuralNetwork += duration
        state.neuralNetworkCalls += 1
        lock.unlock()
    }

    public func addModelLoad(_ duration: Duration) {
        lock.lock()
        state.modelLoad += duration
        lock.unlock()
    }

    public func note(_ shortcut: Shortcut) {
        lock.lock()
        state.shortcuts.insert(shortcut)
        lock.unlock()
    }

    /// NN の計算 1 回を測って `current` に足す（`current` が nil なら測らずに実行するだけ）
    public static func measureNeuralNetwork<T>(_ body: () throws -> T) rethrows -> T {
        guard let timer = current else { return try body() }
        let start = ContinuousClock.now
        defer { timer.addNeuralNetwork(start.duration(to: .now)) }
        return try body()
    }

    /// モデルの読み込みを測って `current` に足す
    public static func measureModelLoad<T>(_ body: () throws -> T) rethrows -> T {
        guard let timer = current else { return try body() }
        let start = ContinuousClock.now
        defer { timer.addModelLoad(start.duration(to: .now)) }
        return try body()
    }
}
