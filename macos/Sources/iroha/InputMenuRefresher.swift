import Carbon
import Foundation

/// バンドルを入れ替えたあと、メニューバーの入力メニュー（TextInputMenuAgent）に入力ソースの一覧を読み直させる。
///
/// `~/Library/Input Methods/iroha.app` が入れ替わると、macOS は入力ソースの情報を作り直す
/// （実測で入れ替えの約0.7秒後と1.3秒後の2回）。その最中に入力メニューが一覧を読むと、
/// iroha を含むキーボード入力ソースが一覧から消えたまま残ることがある（2026-09-26 実測で5〜8回に1回）。
/// TIS の上では有効・選択中のままなので、システム設定から追加し直そうとしてもグレーアウトしていて、
/// ログアウトが必要な状態に見える。
///
/// 入力メニューは選択中の入力ソースが変わると一覧を読み直すので、別のキーボード入力ソースを一度選んでから元に戻す。
/// 作り直しの最中に行うと逆に一覧が壊れる（入れ替えの1.5秒後に行うと15回中10回失敗した）ので、
/// 呼び出し側は新しいプロセスを起動してから数秒おいて呼ぶこと。
/// `killall TextInputMenuAgent` 等では直らない（再起動したメニューも同じ壊れた一覧を読む）
enum InputMenuRefresher {
    static func refresh() {
        let current = TISCopyCurrentKeyboardInputSource().takeRetainedValue()
        let currentID = stringProperty(current, kTISPropertyInputSourceID)
        let filter = [
            kTISPropertyInputSourceCategory as String: kTISCategoryKeyboardInputSource as String,
            kTISPropertyInputSourceIsSelectCapable as String: true,
        ] as CFDictionary
        guard let sources = TISCreateInputSourceList(filter, false)?.takeRetainedValue() as? [TISInputSource]
        else { return }
        // 実測で効果を確かめたのは ABC を経由する形なので、iroha 以外（キーボード配列など）を優先する。
        // 同じ入力ソースを選び直しても TIS は何もしない（メニューに通知が届かない）ので、必ず別のものを挟む
        let others = sources.filter { stringProperty($0, kTISPropertyInputSourceID) != currentID }
        let ownBundleID = Bundle.main.bundleIdentifier
        guard let other = others.first(where: { stringProperty($0, kTISPropertyBundleID) != ownBundleID })
            ?? others.first
        else { return }
        TISSelectInputSource(other)
        usleep(300_000)
        TISSelectInputSource(current)
    }

    private static func stringProperty(_ source: TISInputSource, _ key: CFString) -> String? {
        guard let pointer = TISGetInputSourceProperty(source, key) else { return nil }
        return Unmanaged<CFString>.fromOpaque(pointer).takeUnretainedValue() as String
    }
}
