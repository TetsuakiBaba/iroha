import Foundation
import IrohaCore

/// ローマ字入力の打ち間違いを訂正するモデル（`TypoNormalizer`）の設定。
///
/// 既定OFF。**モデルはアプリに同梱しておらず、ONにした時点でダウンロードする**
/// （`TypoNormalizerDownloader`。重みは本体コード(MIT)と別ライセンスなので配布物を分けてある）。
///
/// ONにすると次の2つのタイミングで訂正が走る:
///
/// 1. **入力中の休止（既定 300ms、`idleDelay`）** — 読みそのものを直し、直した読みで
///    ライブ変換を続ける。iroha はライブ変換が主で、スペースを押さずに確定することも多い。
///    また打ち間違いに気づいた人はスペースではなく Backspace を押すので、
///    「変換を要求した時点」では遅い。直した直後の Backspace で取り消せる
/// 2. **スペース押下（文節変換）** — 休止を待たずに変換した場合の保険。こちらは読みを
///    書き換えず、候補ウィンドウに合流させる
enum TypoNormalizerSettings {

    static let enabledKey = "typoNormalizer"
    static let thresholdKey = "typoNormalizerThreshold"
    static let delayMillisecondsKey = "typoNormalizerDelayMs"

    static let defaultDelayMilliseconds = 300
    static let delayMillisecondsRange = 100...2000

    static var isEnabled: Bool {
        UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? false
    }

    /// 訂正を採用する margin のしきい値。大きいほど過剰訂正が減り、訂正できる数も減る。
    /// 既定は 2.0（SWIFT-PORT.md §4 の推奨。候補ウィンドウに出す設計なら 1.5〜2.0）
    static var threshold: Double {
        guard let value = UserDefaults.standard.object(forKey: thresholdKey) as? Double,
              value.isFinite else { return TypoNormalizer.defaultThreshold }
        return min(10, max(0, value))
    }

    /// 最後の打鍵からこの時間だけ入力がなければ訂正を走らせる。
    /// 人が入力を止める場所は文節や句の切れ目になりやすく、認知的には
    /// 「変換キーを押す地点」に近い（そこがモデルの学習分布にも近い）
    static var idleDelay: Duration {
        let value = UserDefaults.standard.integer(forKey: delayMillisecondsKey)
        let milliseconds = value == 0 ? defaultDelayMilliseconds
            : min(delayMillisecondsRange.upperBound, max(delayMillisecondsRange.lowerBound, value))
        return .milliseconds(milliseconds)
    }

    /// モデルが手元にあるか（設定画面の表示用。無ければ `TypoNormalizerDownloader` が取りにいく）
    static var isModelAvailable: Bool {
        TypoNormalizer.defaultDirectoryURL() != nil
    }

    /// モデルがあるときだけ実体を作る。重みの読み込みは最初の推論まで遅れる
    static func makeNormalizer() -> TypoNormalizer? {
        guard let directory = TypoNormalizer.defaultDirectoryURL() else { return nil }
        return TypoNormalizer(directory: directory)
    }
}
