import Foundation
import IrohaCore

/// 学習用ログ（`ConversionLog`）の設定。
///
/// 既定OFF。左文脈にはアプリのカーソル手前の文章がそのまま入るため、ユーザが自分で選んでONにする。
/// 端末ごとの判断なので `PreferencesSync` の同期対象には入れない（データフォルダを共有している
/// 別のMacで勝手に記録が始まらないように）
enum ConversionLogSettings {

    static let enabledKey = "conversionLogEnabled"

    static var isEnabled: Bool {
        UserDefaults.standard.bool(forKey: enabledKey)
    }
}
