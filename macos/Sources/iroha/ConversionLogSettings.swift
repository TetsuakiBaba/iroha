import Foundation
import IrohaCore

/// 学習用ログ（`ConversionLog`）の設定。
///
/// 既定OFF。左文脈にはアプリのカーソル手前の文章がそのまま入るため、ユーザが自分で選んでONにする。
/// 端末ごとの判断なので `PreferencesSync` の同期対象には入れない（データフォルダを共有している
/// 別のMacで勝手に記録が始まらないように）
enum ConversionLogSettings {

    static let enabledKey = "conversionLogEnabled"
    static let scopeKey = "conversionLogScope"

    /// 何を記録するか
    enum Scope: String, CaseIterable, Identifiable {
        /// すべての確定（修正しなかったものも含む）
        case all
        /// モデルの出力を直した確定だけ
        case corrections

        var id: String { rawValue }

        var label: String {
            switch self {
            case .all: return "すべての確定"
            case .corrections: return "直した確定だけ"
            }
        }
    }

    static var isEnabled: Bool {
        UserDefaults.standard.bool(forKey: enabledKey)
    }

    static var scope: Scope {
        Scope(rawValue: UserDefaults.standard.string(forKey: scopeKey) ?? "") ?? .all
    }

    /// この確定を記録するか。
    ///
    /// 記録しないもの:
    /// - 左文脈がない確定。追加学習は「この文脈でこの読みならこう変換する」を学ぶもので、
    ///   文脈のない例（起動直後やフォーカス移動直後の1語目）は学習データとして役に立たない
    /// - 読みをそのまま確定した1文字（`ConversionLogEntry.isTrivial`。「、」「の」など）
    ///
    /// `edited == false`（モデルの出力をそのまま確定した）を残すかは `scope` で選べる
    static func shouldRecord(_ entry: ConversionLogEntry) -> Bool {
        guard isEnabled, !entry.context.isEmpty, !entry.isTrivial else { return false }
        switch scope {
        case .all: return true
        case .corrections: return entry.edited != false
        }
    }
}
