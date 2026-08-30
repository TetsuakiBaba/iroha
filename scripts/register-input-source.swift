// インストール済みのiroha.appを入力ソースとして登録する（ログアウト不要にするため）
// 使い方: swift scripts/register-input-source.swift
import Carbon
import Foundation

let appURL = URL(fileURLWithPath: NSHomeDirectory() + "/Library/Input Methods/iroha.app")
let status = TISRegisterInputSource(appURL as CFURL)
if status == noErr {
    print("入力ソースを登録しました: \(appURL.path)")
    print("システム設定 > キーボード > 入力ソース > 編集 > + > 日本語 > iroha を追加してください。")
} else {
    print("登録に失敗しました (OSStatus: \(status))")
    exit(1)
}
