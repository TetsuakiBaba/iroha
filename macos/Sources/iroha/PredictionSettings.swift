import Foundation
import IrohaCore

/// 予測変換の設定。
///
/// 入力中、読みの先頭が一致する過去の確定（入力履歴、`InputHistoryStore`）を最大3件、カーソル下の小窓に出す。
/// Tabで選び（押すたびに次の候補）、選んだまま続きを打つか Enter を押すとその候補が入る。クリックでも入る。
/// 入力履歴はこの設定がONの間だけ記録する（打った語がデータフォルダに残るため）
enum PredictionSettings {

    /// 予測変換（既定OFF）。キー名は NN の予測を使っていた頃と同じ
    static let predictiveEnabledKey = "predictiveConversion"

    static var isPredictiveEnabled: Bool {
        UserDefaults.standard.object(forKey: predictiveEnabledKey) as? Bool ?? false
    }

    /// 候補を出し始める読みの文字数。1文字では候補が多すぎて当たらない
    /// （変換記録での試算: 1文字で上位3件に入るのは 26.8%、2文字で 41.6%）
    static let minimumPrefixLength = 2
}
