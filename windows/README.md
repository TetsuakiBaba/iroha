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
iroha-server/   変換サーバ常駐プロセス（llama.cpp + ZenzEngineをホスト）
iroha-core/     変換エンジンのC++移植
                （iroha-core=純粋ロジック / iroha-engine=ZenzEngine+llama.cpp）
                src/lattice/ は辞書ラティス（下の「辞書ラティス」節）
iroha-cli/      検証用CLI（kana / convert / segment / bench / ajimee / lattice /
                lattice-dump / lattice-parity / remote / repl）
shared/         TIP⇔サーバの名前付きパイプIPCプロトコル（ipc_protocol.h）
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

# zenzモデルの取得（%LOCALAPPDATA%\iroha\models へ、約72MB）
powershell -ExecutionPolicy Bypass -File windows\scripts\fetch-model.ps1

# Vulkan対応ビルド（要Vulkan SDK。dist-windows-vulkan / build-vulkan に分離）
powershell -ExecutionPolicy Bypass -File windows\scripts\build-llama.ps1 -Backend vulkan
powershell -ExecutionPolicy Bypass -File windows\scripts\build-tip.ps1 -Vulkan
# 注意: zenz-v3.1-small級の小型モデルでは内蔵GPU（Intel Iris Plus実測）だと
# CPU AVX2より約2倍遅い（n=1: 71ms→157ms）。既定はCPUビルドとし、
# VulkanはディスクリートGPU向けのオプション扱い

# エンジンの検証・評価（macOS版と同じeval.tsvで数値を突き合わせる）
windows\build\iroha-cli\iroha-cli.exe convert kyouhaiitenki
windows\build\iroha-cli\iroha-cli.exe bench testdata\eval.tsv
```

## 辞書ラティス

候補ウィンドウの候補を作る辞書ラティスは、macOS版では AzooKeyKanaKanjiConverter（Swift、MIT）を使う。
Windows版はその変換の部分を C++ に移したもの（`iroha-core/src/lattice/`、入口は
`iroha/lattice_converter.h` の `LatticeConverter`）で、同じ辞書データ（azooKey_dictionary_storage、Apache-2.0）を読む。

```powershell
# 辞書データの取得（vendor/azooKey_dictionary_storage へ。macOS版と同じコミット）
powershell -ExecutionPolicy Bypass -File windows\scripts\fetch-dictionary.ps1

# 生の候補を見る（★は読み全体に一致した候補。モデルは使わない）
windows\build\iroha-cli\iroha-cli.exe lattice きしゃ

# macOS版の候補の並びと一致するかを確かめる（移植が正しいかの判定）。
# macOS側で `iroha-cli lattice-dump <読みの一覧> <出力.jsonl> --n 10` を実行し、その出力を渡す
windows\build\iroha-cli\iroha-cli.exe lattice-parity <macOS版の出力.jsonl> --n 10
```

- 辞書フォルダは、環境変数 `IROHA_DICTIONARY` → 実行ファイルと同じフォルダの `Dictionary` →
  カレントディレクトリから上へたどった `vendor/azooKey_dictionary_storage/Dictionary` の順に探す
- 移してあるのは macOS版の `LatticeConverter.swift` が使う経路だけ（かなを直接渡す・誤り訂正なし・
  予測変換なし・azooKey自身の学習なし）。**専門辞書（azooKeyのユーザ辞書の形式に書き出して読ませる部分）は未移植**
- 確認（2026-11-13）: macOS版が同じ版（v0.11.2・辞書コミット 832fbb0）で書き出した AJIMEE-Bench 200件の
  候補一覧（`git show v0.12.0:experiments/jev/ajimee-candidates.jsonl` と `…-n30.jsonl`）と、
  候補の並びまで 200/200 件が一致。速度は iroha-bench-v1 の 3,253件で平均 4.3ms（最大 54ms）
- AzooKeyKanaKanjiConverter の版を上げるときは、`src/lattice/` を新しい版に合わせて直し、
  `fetch-dictionary.ps1` の `$DictCommit` を macOS版の `fetch-dictionary.sh` と合わせ、`lattice-parity` で確かめる
- 元の実装の和暦の候補には、明治の年の計算に書き間違いとみられる箇所がある（「1900ねん」→「明治-67年」）。
  両方で直してある（2026-11-13。macOS版は `EraYearCandidates.swift`、Windows版は `special_candidates.cpp`。
  規則を変えるときは両方を合わせる）。これより前に書き出した macOS版の出力と突き合わせると、明治の年の読みだけ違いが出る

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
- [x] M3: コンポジション表示（RomajiComposer移植、下線、Enter確定/Esc破棄）
- [x] M4a: 変換サーバ接続（iroha-server + パイプIPC。Spaceで変換、Space/↑↓で候補送り、
      Esc/Backspaceで読みに戻る、TIP有効化時にサーバ自動起動+モデルプリロード）
- [x] M4b: 自前候補ウィンドウ（GetTextExt座標に表示、数字キー1-9で選択、
      EVENT_OBJECT_IME_*発火、フォーカス非奪取）と左文脈の伝搬（コンポジション直前40文字）
- [x] M4c: 入力モード切替（半角/全角・Alt+`でかな⇔英数トグル、ひらがな/英数キー対応。
      モード表示アイコンはM5の通知領域対応で追加する）
- [x] M4d: 学習・ユーザ辞書の移植（学習→ユーザ辞書→zenzのデコレータ構成をサーバに搭載。
      確定がエンジンの第一候補と違うときだけTIPからの通知で学習を記録。
      learning.json / user-dictionary.json はmacOS版と同形式、%LOCALAPPDATA%\iroha に保存）
- [x] M4e: 文節操作（変換後は文節モード: ←→で文節移動［選択文節は太下線］、
      Shift+←→で区切り調整＋再変換、Space/↑↓で選択文節の候補送り、
      候補は「文節の読み＋左側の確定済み文字列」を文脈に生成、Enterで一括確定）
- [x] M5a: ストアアプリ対応と通知領域アイコン
      （パイプにAppContainer/低整合性からの接続を許可、ログオン時のサーバ自動起動
      ［HKCU Run。AppContainer内のTIPはプロセスを起動できないため］、
      言語バーの入力モードボタン「あ/A」＝クリックで切替・テーマ連動アイコン）
- [x] M6a: 設定ウィンドウ（iroha-settings.exe: モデル選択・ダウンロード・切替、
      ユーザ辞書の一覧/追加/削除、学習リセット。あ/Aボタン右クリックで起動。
      config.json + IPCのReloadでサーバへ即時反映）と配布用バンドル（make-bundle.ps1）
- [x] M7a: 辞書ラティスのC++移植（`iroha-core/src/lattice/`。macOS版の候補の並びと一致を確認。
      サーバ・候補ウィンドウへの組み込みと、モデルでの再採点［LatticeRescoringEngine］は未実装）
- [ ] M5b: UIlessモード（ITfCandidateListUIElement。タスクバー検索ボックス等）
- [ ] M6b: 製品化の残り（インストーラ/署名、release.ymlの両OS対応、UIA対応、
      ライブ変換［エンジン準備済み・TIP非同期化が未実装］）

参考実装: CorvusSKK（構成の手本）、Microsoft textservice step01-06（チュートリアル）、
SampleIME（登録・UIlessの正解集）、mozc `src/win32/tip/`（完成形）。
