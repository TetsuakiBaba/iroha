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

# 既定の変換モデル iroha-kkc-v1 の取得（%LOCALAPPDATA%\iroha\models へ、約82MB。SHA-256 を照合する）
powershell -ExecutionPolicy Bypass -File windows\scripts\fetch-model.ps1
# 以前の既定（zenz-v3.1-small）を比較用に取得する
powershell -ExecutionPolicy Bypass -File windows\scripts\fetch-model.ps1 -Model zenz-v3.1-small

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

## 変換モデル（ZenzEngine）

`iroha-engine` の `ZenzEngine` は macOS版の `ZenzEngine.swift` を移したもの。デコーダ専用のモデル（zenz）と
エンコーダ・デコーダ型のモデル（既定の iroha-kkc-v1、文字単位 T5）を同じプロンプト形式で動かす。
生成（第一候補・n-best）・候補の採点（`Score`）・LoRA アダプタ・読み制約の設定に対応。
文字ごとの自信度（`convertWithConfidence`。「AIで直す」の判定に使う）は未移植。

```powershell
# モデルの生成と採点を macOS版の出力（iroha-cli ajimee-dump）と突き合わせる。同じモデルを IROHA_MODEL で指定する
windows\build\iroha-cli\iroha-cli.exe model-parity <macOS版の出力.jsonl>
# 採点が生成と辻褄が合うかを確かめる（macOS版の出力は使わない。入力は同じ形式の JSONL）
windows\build\iroha-cli\iroha-cli.exe model-selfcheck <同じ形式.jsonl> --limit 40
# 2条件での評価（モデルのみ / 辞書ラティス + モデルの再採点）
$env:IROHA_LATTICE = "off";    windows\build\iroha-cli\iroha-cli.exe ajimee testdata\ajimee\evaluation_items.json
$env:IROHA_LATTICE = "always"; windows\build\iroha-cli\iroha-cli.exe ajimee testdata\ajimee\evaluation_items.json
```

- **候補の採点は1つずつ行う（macOS版と違う）。**macOS版はプロンプトのKVキャッシュを候補ごとの系列へ複製し、
  全候補を1バッチで採点する。llama.cppのCPU版は、1バッチに載せるトークン数（64以上で別の計算経路）や
  キャッシュ上の位置で値が変わり、同じ候補でも対数確率が 0.1〜0.9 動く（2026-11-13 実測。同じ候補を2つ並べると
  2つが違う値になる。候補どうしが見えているわけではなく、並べる候補の長さだけで変わる）。Windows版は
  プロンプトを1回だけ評価してその直後のlogitsを取っておき、候補ごとにKVキャッシュをプロンプトだけの状態に
  戻して続きに並べる。これで生成と同じ経路になり、生成した文の採点が生成のときの対数確率と一致する
  （`model-selfcheck` が差 0.00000）。同じプロンプトでの n-best の分岐・変換のあとの採点も、プロンプトを評価し直さない
- 確認（2026-11-13、このWindows機のCPU・4スレッド）:

  | モデル | 評価 | 結果 | macOS版の記録 |
  |---|---|---|---|
  | zenz-v3.1-small | macOS版の出力との照合（AJIMEE 200件） | 生成の一致 196/200、最良の候補の一致 198/200 | — |
  | zenz-v3.1-small | AJIMEE モデルのみ / ラティス再採点 | 169 (84.5%) / 169 (84.5%) | 171 (85.5%) / 169 (84.5%) |
  | zenz-v3.1-small | eval.tsv 40件 | 36 (90.0%)、89ms | 36 (90.0%) |
  | iroha-kkc-v1 | AJIMEE モデルのみ / ラティス再採点 | 171 (85.5%) / 171 (85.5%)、221ms / 386ms | （手元に記録なし） |
  | iroha-kkc-v1 | eval.tsv 40件 | 38 (95.0%)、85ms | （手元に記録なし） |

  macOS版（Metal）とは計算機が違うので、採点の値は 2% ほどずれる（順位はほぼ同じ）
- 既定のモデルは `iroha-core` の `config.h`（`kDefaultModelFileName`）。未取得で以前の既定（zenz-v3.1-small）が
  あればそれを使う。差し替えるときは `fetch-model.ps1`・`iroha-settings` のモデル一覧と、macOS版の
  `ModelDownloader.swift`・`fetch-model.sh` を合わせる
- 変換サーバは「学習 → ユーザ辞書 → 辞書ラティスの候補をモデルで再採点（`LatticeRescoringEngine`）→ モデル」の順に重ねる。
  候補を複数求められたとき（候補ウィンドウ）だけ辞書ラティスを使い、全体の変換（候補1つ）はモデルの生成に任せる。
  辞書は実行ファイルと同じフォルダの `Dictionary`（`install-tip.ps1`・配布zipの `install.ps1` がコピーする）。
  学習・ユーザ辞書は v0.5 時点の仕様のまま（macOS版の今の仕様への追随は未実装）。長い読みの分割・異体字も未移植
- 候補ウィンドウは9件ずつのページで出す（数字キーはページの中の番号。右下に「2/7」）

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
- [x] M7a: 辞書ラティスのC++移植（`iroha-core/src/lattice/`。macOS版の候補の並びと一致を確認）
- [x] M7b: モデルの採点・T5（iroha-kkc-v1）対応と、辞書ラティスの候補の再採点（`LatticeRescoringEngine`）。
      変換サーバに組み込み、既定のモデルを iroha-kkc-v1 に変更。候補ウィンドウのページ送り
- [ ] M7c: 学習・ユーザ辞書を macOS版の今の仕様に合わせる（直す前の結果つきの学習、ユーザ辞書の語の境目）、
      長い読みの分割（ChunkedConversionEngine）、異体字（VariantKanjiEngine）、記号・絵文字の候補、専門辞書
- [ ] M5b: UIlessモード（ITfCandidateListUIElement。タスクバー検索ボックス等）
- [ ] M6b: 製品化の残り（インストーラ/署名、release.ymlの両OS対応、UIA対応、
      ライブ変換［エンジン準備済み・TIP非同期化が未実装］）

参考実装: CorvusSKK（構成の手本）、Microsoft textservice step01-06（チュートリアル）、
SampleIME（登録・UIlessの正解集）、mozc `src/win32/tip/`（完成形）。
