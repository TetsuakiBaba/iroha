# iroha for Windows

WindowsのIME（TSF: Text Services Framework）実装。方針（2026-09決定）:

- 変換エンジンは **C++で再実装**（正は `macos/Sources/IrohaCore/` とREADME「開発」節。
  移植した62テスト + `testdata/eval.tsv` のbenchでmacOS版と挙動一致を確認する）
- 構成は **TIP DLL（極小・依存最小）+ 変換サーバ常駐プロセス**（mozc/CorvusSKK方式）。
  TIPは入力先の全プロセスにロードされるため、llama.cpp・モデル・エンジンは
  必ずサーバプロセス側に置き、名前付きパイプでIPCする
- 対応アーキテクチャは当面 **x64のみ**（32bitアプリでは使えない。x86/ARM64は後回し）

## 構成

```
iroha-tip/      TSFテキストサービスDLL（C++/COM。変換ロジックを持たない）
iroha-server/   変換サーバ（llama.cpp + エンジン。今後実装）
iroha-core/     変換エンジンのC++移植（今後実装）
shared/         TIP/サーバ共通のIPCプロトコル定義（今後実装）
scripts/        ビルド・インストールスクリプト
```

## ビルドと動作確認

```powershell
# llama.cpp静的ライブラリ（vendor/dist-windows へ。vendor/dist はmacOS用なので触らない）
powershell -ExecutionPolicy Bypass -File windows\scripts\build-llama.ps1

# TIP DLL
powershell -ExecutionPolicy Bypass -File windows\scripts\build-tip.ps1

# インストール（管理者PowerShellで。Program Files配置 + regsvr32登録）
powershell -ExecutionPolicy Bypass -File windows\scripts\install-tip.ps1
# 解除
powershell -ExecutionPolicy Bypass -File windows\scripts\install-tip.ps1 -Uninstall
```

インストール後、設定 > 時刻と言語 > 言語と地域 > 日本語 > キーボード に
「iroha」が現れ、Win+Space で切り替えられる（出ない場合はサインアウト）。

## 開発メモ

- ビルド環境: VS 2022の「C++によるデスクトップ開発」+ cmake/ninja。
  スクリプトはvcvarsに頼らずINCLUDE/LIB/PATHを直接組み立てる
  （この環境ではCMakeのVS検出・vcvars64.batが動かないため）
- ログ: TIPはホストプロセス内で動くため `OutputDebugString` に出す。
  Sysinternals **DebugView** で全プロセス横断で拾える（フィルタ: `[iroha-tip]`）
- デバッグ: VSの「プロセスにアタッチ」でnotepad.exeにアタッチしてブレーク
- DLL差し替え: 使用中の全プロセスにロックされる。
  「MS-IMEへ切替 → テスト用アプリ終了 → install-tip.ps1」。掴まれていたらサインアウト
- **TIPから例外を絶対に漏らさない**（クラッシュ＝ホストアプリのクラッシュ）
- .ps1はUTF-8 **BOM付き**で保存する（PowerShell 5.1がBOMなしを誤読する）

## マイルストーン

- [x] M1: 登録できる空TIP（設定に現れWin+Spaceで選べる、Activate/Deactivateがログに出る）
- [x] M2: キーを食って文字を挿入（ITfKeyEventSink + エディットセッション）
- [x] M3: コンポジション表示（RomajiComposer移植、下線、Enter確定/Esc破棄。Spaceは仮確定）
- [ ] M4: 変換サーバ接続 + 候補ウィンドウ（iroha-core + llama.cpp、名前付きパイプIPC）
- [ ] M5: 互換性強化（UIlessモード、ストアアプリ/AppContainer、通知領域アイコン）
- [ ] M6: 製品化（インストーラ、Authenticode署名、学習・ユーザ辞書、UIA対応）

参考実装: CorvusSKK（構成の手本）、Microsoft textservice step01-06（チュートリアル）、
SampleIME（登録・UIlessの正解集）、mozc `src/win32/tip/`（完成形）。
