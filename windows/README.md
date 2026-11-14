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
iroha-tip/      TSFテキストサービスDLL（C++/COM。変換ロジックも入力の状態も持たない）
iroha-server/   変換サーバ常駐プロセス（llama.cpp + ZenzEngineをホスト。入力の状態もここに持つ）
iroha-core/     変換エンジンのC++移植
                （iroha-core=純粋ロジック / iroha-engine=ZenzEngine+llama.cpp）
                src/lattice/ は辞書ラティス（下の「辞書ラティス」節）
                input_session / session_manager は入力の状態（下の「入力の状態とライブ変換」節）
iroha-cli/      検証用CLI（kana / convert / segment / bench / ajimee / lattice /
                lattice-dump / lattice-parity / remote / session / symbols / repl）
shared/         TIP⇔サーバの名前付きパイプIPCプロトコル（ipc_protocol.h）と、
                サーバ・CLI で使う Windows 専用の部品（emoji_renderability）
scripts/        ビルド・インストールスクリプト
```

## ビルドと動作確認

```powershell
# llama.cpp静的ライブラリ（vendor/dist-windows へ。vendor/dist はmacOS用なので触らない）
powershell -ExecutionPolicy Bypass -File windows\scripts\build-llama.ps1

# 辞書ラティスの辞書と、記号・顔文字・絵文字のデータ（vendor/ へ。macOS版と同じコミット）
powershell -ExecutionPolicy Bypass -File windows\scripts\fetch-dictionary.ps1
powershell -ExecutionPolicy Bypass -File windows\scripts\fetch-symbols.ps1

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
# VulkanはディスクリートGPU向けのオプション扱い。
# iroha-kkc-v1（T5）でも同じ（2026-11-13、Core i7-1065G7 / Iris Plus、IROHA_LATTICE=off）:
#   eval.tsv 40件  CPU 56〜74ms → Vulkan 167〜171ms（出力は同じ 38/40）
#   AJIMEE 200件   CPU 163ms（172/200）→ Vulkan 259ms（171/200）
# 読みが長いほど差は縮むが、内蔵GPUでは逆転しない。ディスクリートGPUでは未計測。
# Vulkanビルドのまま IROHA_GPU_LAYERS=0 でCPU推論にできる（eval.tsv 61ms。CPUビルドと同じ速さ）

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
  予測変換なし・azooKey自身の学習なし）。専門辞書は下の「専門辞書」節
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
文字ごとの自信度（macOS版の `convertWithConfidence`。「AIで直す」の判定に使う）は `ConvertScored` が返す。

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
- 変換サーバは「学習 → ユーザ辞書 → 長い読みの区切り（`ChunkedConversionEngine`、80文字超）→ 異体字（`VariantKanjiEngine`）→
  辞書ラティスの候補をモデルで再採点（`LatticeRescoringEngine`）→ モデル」の順に重ねる（macOS版 iroha-cli の makeEngine と同じ順）。
  候補を複数求められたとき（候補ウィンドウ）だけ辞書ラティスを使い、全体の変換（候補1つ）はモデルの生成に任せる。
  辞書は実行ファイルと同じフォルダの `Dictionary`（`install-tip.ps1`・インストーラーが置く）。
  学習とユーザ辞書は macOS版の今の仕様（2026-11-13 に合わせた）:
  学習は「読み全体 → 確定文字列」と「直す前にエンジンが出していた結果」を覚え、エンジンがまた同じ結果を出したときだけ
  差し替える（差し替えた結果をエンジンの結果に戻して確定したら消す。文節単位の学習は読み込み時に捨てる）。
  ユーザ辞書は、読みの途中の一致を辞書ラティスで語の境目になる所だけ使い、単語が読みより大幅に長いエントリは
  候補ウィンドウにだけ出す。learning.json・user-dictionary.json は macOS版と同じ形式。
  ほかの端末が書いたファイルの取り込み（macOS版の `reloadIfChanged`）は未移植
- 候補ウィンドウは9件ずつのページで出す（数字キーはページの中の番号。右下に「2/7」）

インストール後、設定 > 時刻と言語 > 言語と地域 > 日本語 > キーボード に
「iroha」が現れ、Win+Space で切り替えられる（出ない場合はサインアウト）。

## 入力の状態とライブ変換

入力の状態（読み・ライブ変換の結果・文節・候補）は **変換サーバ側** に持つ（2026-11-13）。
TIP はアプリのプロセスの中で動くので、判断と状態を置かない。

- `iroha-core` の `InputSession` が状態機械（キーごとに、確定する文字列・未確定文字列・候補を決める）。
  OS にも通信にも依存しないので、偽の変換を渡して単体テストできる（`tests/test_session.h`）。
  ライブ変換の表示と確定の規則は macOS版 `IrohaInputController` の `compositionDisplay` / `resolveCommitText` と同じ:
  変換が届くまでは「前の変換結果 + 増えたかな」を出し、文字を消したら次の文字までかな表示、
  1 回目の Esc でかな・2 回目で取り消し。文節変換は、これまで TIP にあった動きを移したもの
- `SessionManager` がセッションの表（TIP ごとに 1 つ。番号は TIP が有効化のたびに作る）と、ライブ変換を行う
  裏のスレッドを持つ。キーの処理はすぐ戻り、ライブ変換は裏で進める。エンジンは同時に 1 つの変換しか行えないので、
  キーの処理が変換を必要としたとき（Space・候補・文節の区切り）は、進行中のライブ変換をやめさせて先に行う
  （`ZenzEngine::SetCancelCheck`。生成の 1 トークンごとに確かめる）
- 新しいライブ変換の依頼では、進行中の変換をやめさせない。その結果は、読みが伸びたあとでも頭の部分の変換として
  表示に使う（macOS版は新しい入力のたびに前の変換を捨てる。CPU で動かす Windows 版では、変換が打鍵に
  追いつかないときに表示が進まなくなるので変えた）。始めていない古い依頼は捨て、最新の読みだけを変換する
- TIP（`TextService`）が行うのは 3 つ: キーコードを意味に読み替えて受けるかどうかを決める・キーを送って返ってきた
  表示（`SessionOutput`）を文書に反映する・ライブ変換の結果待ちの間、15ms のタイマーで変換サーバに問い合わせる
  （`Polling.cpp`。届いたら非同期のエディットセッションで反映する）。
  受けるかどうかは OnTestKeyDown ですぐ答える必要があるので、直近に受け取った表示で決める:
  文字は常に、数字と Enter / Esc / Backspace / Space は合成中だけ、矢印は文節変換中だけ
- 左文脈は、合成を始めるキーを送るときに TIP がカーソルより前の 64 文字を読んで付ける。変換サーバが
  カーソルのある行だけ・末尾 40 文字に整える（macOS版の `LeftContext.normalize`）。読めないアプリでは、
  直前に確定した文字列を使う
- 通信（`shared/ipc_protocol.h`）は、メッセージの種類を足すだけにして版を変えていない。アプリに読み込まれたままの
  古い TIP は、新しい変換サーバとも Convert / Record で動く
- ライブ変換は設定画面の「入力」タブ（`config.json` の `"liveConversion"`）で止められる

```powershell
# 変換サーバのセッションにキーを順に送り、TIP が画面に出す内容を見る（インストールせずに確かめられる）。
# IROHA_PIPE_SUFFIX を付けると、インストール済みの変換サーバとは別の名前で動く
$env:IROHA_PIPE_SUFFIX = "dev"
Start-Process windows\build\iroha-server\iroha-server.exe
windows\build\iroha-cli\iroha-cli.exe session "kyouhaiitenkidesune<space><right><space><enter>"
# 待たずに 120ms おきに送る（変換が打鍵に追いつかないときの表示）
windows\build\iroha-cli\iroha-cli.exe session --interval 120 "kyouhaiitenkidesune<enter>"
```

確認（2026-11-13、このWindows機のCPU・iroha-kkc-v1）: ライブ変換は読み 10 文字前後で 30〜60ms で届き、
キーへの返事は 1ms 未満。120ms おきの打鍵でも表示は 1〜2 文字遅れで付いてくる。
**TIP の側（文書への反映・タイマー）は、インストールして実際のアプリで確かめる必要がある**

キーの扱いは macOS版に合わせてある（2026-11-13）:

- Space 1 回で文節に分けて最初の文節の候補ウィンドウまで開く。Space/↓ で次、↑ で前（Shift+Space も前。
  Windows の IME の慣習で、macOS版には無い）、数字キーで選んでウィンドウを閉じる、Esc はウィンドウを閉じて、
  もう 1 度でかなに戻る、Enter で全体を確定
- 文節変換中に文字を打つと、選んでいる文節をその文字から打ち直す（範囲の読み全体を裏で変換し直す）。
  Backspace は選んでいる文節の読みを後ろから消す。Esc で打ち直す前に戻す。打ち直した確定は学習しない
- F6〜F10・Ctrl+U/I/O/P/T で表示形（ひらがな・カタカナ・半角カナ・全角英数・半角英数）。続けて打つと、
  指定した部分は固定され、残りだけをライブ変換する。学習で差し替えた表示を退けて確定したら、その学習を消す
- Shift+英字（大文字）で英字入力。Shift を押している間は記号・数字も英字に足し、離して打つとかなに戻る
  （「今日はAIを使う」を 1 回の Enter で確定できる）
- 確定した直後の Ctrl+Backspace で確定する前の未確定文字列に戻す。TIP はカーソルの手前が確定した文字列のままか
  確かめてから取り消す（違えばキーをアプリに渡す）。確定に伴う学習は、取り消せなくなったとき（次のキー）に書く
- Ctrl+. で句読点（、。⇄，．）を切り替える（`config.json` の `punctuation` に保存する）。入力していないときの Space は
  全角（`"fullWidthSpace": false` で半角）。合成中の矢印は握りつぶし、IME が扱わないキー（Tab・Ctrl+S など）は
  確定してからアプリに渡す（**TSF で OnKeyDown が「食わない」と返したキーがアプリに届くことは、実機で確かめる**）
- 候補の数は設定画面の「入力」タブ（`config.json` の `candidateCount`、既定 8。macOS版と同じ）
- 区切り記号で囲んだ英字入力（macOS版と同じく開発者モードの項目。設定 > 入力 の「区切り記号で英字を入力」、`delimitedAlphabet`）:
  かな入力のまま「_」から「_」までを打ったとおりの英字にする（「kyouha_iPhone_wo」→「今日はiPhoneを」）。Shift+Space で
  英字の中にスペース、「__」で記号そのもの。記号は `delimitedAlphabetStart`・`delimitedAlphabetEnd` で変えられる
- 候補ウィンドウは 9 件ずつの列で、候補が 1 列に収まらないときは Tab で列を横に並べた表に広げる（もう 1 度で 1 列に戻す）。
  表に広げている間だけ ←→ は隣の列へ移る（macOS版の `CandidateGrid` を `iroha-core` の `candidate_grid.h` に移した。
  テストは 1 対 1）

変換サーバが止まっている間は、かな入力もできない（キーを受けても表示を決められない）

## 予測変換・変換ルール・プライベートモード・変換記録

macOS版の同じ機能を移した（2026-11-13）。ファイルの形式は macOS版と同じで、設定画面の「入力」「変換ルール」
「辞書・学習」のページから切り替え・編集する（変更は Reload で変換サーバにすぐ効く）。

- 予測変換（既定 OFF）: 入力履歴（`%LOCALAPPDATA%\iroha\input-history\<コンピュータ名>.json`、`InputHistoryStore`）から、
  読みの先頭が一致する語を、読みが 2 文字以上のときにカーソルの下の小窓に出す。Tab・↓ で選び、選んだまま打つと
  その語が入って続きになる。Backspace・Esc で打っていた読みに戻す。入力履歴は予測変換が ON の間だけ記録する
- 変換ルール（`user-rewrite-rules.json`、`UserRewriteRule`）: 読みが一致したら、出力のテンプレート（`{{date}}`・
  `{{date+1}}`・`{{time:HH:mm}}`・`{{wareki}}` など）を展開して文節の候補の 2 番目に足す。初めて読むときに既定のルール
  （きょう・あした・いま など）を入れる。ルールの出力を選んだ確定は学習しない
- プライベートモード（Ctrl+Alt+P・設定画面）: 学習・入力履歴・予測変換・変換記録・ユーザ辞書を止める。
  切り替えたらカーソルの近くに知らせを出し、言語バーの「あ/A」に下線を引く。モード中に確定した文字列は、
  モードを切ったあとの左文脈に使わない
- 変換記録（既定 OFF、`conversion_log`）: 確定を `logs\conversions\conversions-<コンピュータ名>-YYYY-MM.jsonl` に
  1 行ずつ残す（追加学習用のデータ。項目と並びは macOS版の `ConversionLog` と同じ）。左文脈のない確定・読みを
  そのまま確定した 1 文字・打ち直した文節や学習しない候補を含む確定は残さない。「直した確定だけ」も選べる。
  確定の取り消し（Ctrl+Backspace）で取り消した確定は残さない（取り消せなくなったときに書く）
- 設定画面を特定のページで開く: `iroha-settings.exe --tab input|learning|dictionary|terms|rules|selection|ai|model|about`

## 設定画面

2026-11-14 に Windows 11 の設定アプリの見た目に作り直した（ユーザの判断）。左に項目の一覧（入力・辞書・学習・ユーザ辞書・
専門辞書・変換ルール・選択テキスト・AI・モデル・情報）、右に節の見出しと、項目のカード（題・灰色の短い説明・右側の操作部品）を
並べる。部品は `iroha-settings/ui.cpp`（スイッチ・「?」の説明の小窓・スクロールするページ・「開発者」の印。描画は GDI+）

- 項目の細かな説明は画面に並べず「?」に入れる（macOS版の `HelpButton` と同じ考え方。文面も macOS版に合わせた）。
  状態・注意（モデルの有無・読めないショートカットなど）は説明の行に出す
- 変えたらすぐ `config.json` に保存して変換サーバに Reload を送る（保存のボタンは無い。入力欄は欄を離れたときに保存する）
- 開発者モード（情報 > 開発者向け。`config.json` の `developerMode`、既定 OFF）: macOS版の開発者モードの項目のうち Windows に
  ある機能を同じ場所に出し、紫の「開発者」の印を付ける（見出しに印があればその節全体）。OFF の間は表示しないだけで、値は効く。
  入力: アプリの文章を文脈に使う（`documentContext`）・訂正モデル／休止時間／最低文字数／確信の強さ・区切り記号で英字を入力・
  AIで直す、辞書・学習: 記号・顔文字・絵文字、選択テキスト: 除外するアプリ、モデル: モデルファイルのパス・LoRA アダプタ
  （`modelAdapterPath`。変換サーバがモデルに重ねる）
- 一般の項目として足したもの: 変換の学習を使う（`learningEnabled`）・プライベートモードのショートカット（`privateModeHotkey`、
  既定 Ctrl+Alt+P。TIP は変換サーバの返事で受け取る）・選択テキストのマウスのきっかけと文字数の表示・版・データの保存場所・ライセンス
- macOS版にあって作っていない項目: ¥キーで入力する文字・推論時間の表示（開発者モード）・入力履歴と学習と変換記録の一覧の編集・
  アップデートの確認・データの保存場所の変更・アンインストール（macOS 専用の項目は除く）

## AI変換

macOS版の AI変換（入力中にプリセットのキーで、未確定文字列をプロンプトといっしょに AI に渡す）を移した（2026-11-13）。
接続先は Ollama・LM Studio・OpenAI互換・Claude（Apple Intelligence は macOS 専用）。設定画面の「AI」タブで選ぶ

- 依頼の組み立て（`{text}` の差し込み・出力の念押し）・プリセットの既定・応答の読み方（OpenAI互換の SSE・Ollama の JSONL・
  Claude の Messages API のストリーム・`<think>` の除去）は `iroha-core` の `ai_conversion`、通信は `shared/ai_client`（WinHTTP）。
  送る中身と思考を止める指定は macOS版の `RemoteTranslator` と同じ。30 秒何も届かなければ打ち切る（macOS版と同じ）
- 入力の流れは macOS版と同じ: 結果待ちの間は「日本語 ⇢ 途中までの結果 …」を出し、Esc で取り消して元の未確定文字列に戻る
  （ほかのキーは飲み込む）。英訳など日本語でない結果は確定し、日本語の結果は文節に区切って返す（Enter で確定、Space・矢印で
  文節ごとに直せる）。失敗したら日本語のまま確定する。AI の結果は学習・変換記録・入力履歴に残さない
- プリセットは 3 つ（英訳・敬語・要約。名前・プロンプト・キーを変えられる）。キーは「Alt+Enter」「Ctrl+Shift+T」の形で書き、
  英訳の既定は Shift+Space（2026-11-14 にユーザの指定で Alt+Enter から変更。macOS版は Option+Return）、敬語・要約は空。
  変換サーバがキーを解釈して TIP に渡す（TIP は設定ファイルを読めないアプリの中でも動くため）。
  Shift+Space だけは TIP が Space として送り、`InputSession` が決める: 候補ウィンドウを開いているときは前の候補、
  区切り記号の英字入力中は英字の中のスペースのままにし、それ以外の入力中・文節変換中は AI変換にする
- API キー（OpenAI互換・Claude）は config.json に書かず、Windows の資格情報マネージャー（汎用資格情報 `iroha/openAIAPIKey`・
  `iroha/anthropicAPIKey`）に置く。Claude の既定のモデルは claude-haiku-5-5（macOS版と同じ）
- 確認（2026-11-13、この機械の LM Studio・google/gemma-3n-e4b）: 英訳・敬語・要約が途中経過つきで返り、変換サーバを通して
  英訳の確定・日本語の結果の文節化・Esc での取り消しが動いた。Claude・OpenAI・Ollama の実際の接続は試していない

```powershell
# 設定画面の接続先で 1 回依頼する（途中経過も出す）・モデルの一覧
windows\build\iroha-cli\iroha-cli.exe ai "今日は天気がいいですね"
windows\build\iroha-cli\iroha-cli.exe ai --preset 1 "明日の会議、出られないかも"
windows\build\iroha-cli\iroha-cli.exe ai models
# 変換サーバを通して（<ai0> がプリセット 1 つ目のキー）
windows\build\iroha-cli\iroha-cli.exe session --interval 60 "kyouhatenkigaiidesune<wait><ai0><wait>"
```

### 選択テキストのAI編集

macOS版の「選択テキストのAI編集」（ほかのアプリで選んだ文章を、グローバルショートカットで AI に渡して置き換える）を移した
（2026-11-13。設定画面の「選択テキスト」タブ。既定 OFF）。変換サーバの中の専用のスレッドと窓で動く（`iroha-server/selection_assistant.cpp`）

- 選択の取り方は macOS版と違い、クリップボード経由だけにした（ユーザの判断）: クリップボードの中身を控えて Ctrl+C を送り、
  結果は元のアプリに戻って Ctrl+V で貼り、控えた中身を戻す。書き込む一時の中身には、クリップボードの履歴（Win+V）と
  クラウドの同期に残さない印を付ける。ビットマップなど GDI の取っ手で渡される形式は控えられない
- プリセットは 5 つ（日英翻訳・校正・敬語・要約・カスタム。名前・プロンプト・キー・使うかを変えられる）と、その場で指示するキー。
  キーの既定は macOS版の Ctrl+1〜4・Ctrl+0 が Windows ではブラウザのタブ切り替えなどと重なるので Ctrl+Alt+1〜4・Ctrl+Alt+0
  にした。選択が無ければ指示から文章を作ってカーソルの位置に入れる。入力中（未確定文字列があるとき）は動かない（macOS版と同じ）
- 結果の窓は Enter で置き換える・Esc で閉じる・コピー。出力の整形（コードの囲み・全体を包む引用符を外す）は macOS版の
  `AIOutputCleaner` と同じ規則（`iroha-core` の `CleanAIOutput`）
- マウスで選んだとき（ドラッグ・ダブルクリック）は、macOS版と同じくマウスの近くにアイコンを出し、クリックでプリセットのメニューを
  開く（2026-11-14。出し方は オフ／アイコン（既定）／すぐにメニュー／Alt を押しながら選んだときにメニュー。`selectionTriggerMode`）。
  マウスは低水準のフック（専用のスレッド。そこでは何も待たない）で見て、選んだ文章は UI Automation の TextPattern で読む
  （クリップボードは使わない）。読めないアプリでは、入力欄・文書の上で選んだときだけアイコンを出し、メニューを選んだ時点で
  Ctrl+C で受け取る。選んだ文字数をアイコンに添えて出せる（`selectionCharacterCount`。macOS版の `CharacterCount` を移した）
- アプリは起動したときの TIP を使い続ける（入れ直しても、開き直すまで前の版のまま）。TIP がキーと一緒に版を送り、変換サーバは
  前の版の TIP を読み込んだままのアプリの名前を、マウスの近くに 1 度だけ知らせる（`kTipRevision`）
- **キーを送る部分（Ctrl+C・Ctrl+V）と結果の窓は、実際のアプリでまだ確かめていない**（この機械を使っている最中に、ほかのアプリへ
  キーを送る確認はしなかった）。変換サーバが起動してショートカット 5 つを登録できることまで確かめた

### AIで直す

macOS版で開発者モードにだけある「AIで直す」も移した（2026-11-13。設定 > 入力 の開発者モードの項目、`aiFix`。既定 OFF）。
入力が止まって 500ms たったら、モデル単体でもう一度変換して文字ごとの自信度（読み制約を満たす 1 位と 2 位の
トークンの対数確率の差。`ZenzEngine::ConvertScored` が返す）を求め、表示中の変換と同じで最小の差が閾値
（`aiFixThreshold`、既定 1.0）より小さければ、カーソルの下に「AIで直す（Ctrl+Alt+A）」を出す。キー（`aiFixShortcut`）を押すと、
前の文脈・読み・変換結果を AI変換と同じ接続先に送り、案を文節に区切って差し替え、「今の変換 → AI の案」を出す。
直後の Backspace で元の変換に戻す。キーは窓が出ていなくても、入力中ならいつでも使える

- 規則（疑いの判定・依頼の文面・答えの読み方）は `iroha-core` の `ai_fix`（macOS版の `AIFixRule` と同じ）。
  判定はライブ変換が終わるのを待ってから変換エンジンを使う（ライブ変換を止めない）
- macOS版の窓はクリックでも送れるが、Windows版はキーだけ（候補ウィンドウと同じく、フォーカスを取らない窓のため）
- 確認（2026-11-13）: 「きしゃがきた」→「汽車が来た」は「汽」の差が 0.55 で窓が出た。この機械の LM Studio に送って
  答えが同じときの知らせまで動いた（`iroha-cli confidence <読み>` で文字ごとの値を見られる）

## 「こえ」モード

macOS版の「こえ」モード（決めたキーを押している間だけ録音し、離したら認識結果を読みに戻して未確定文字列に入れる）を移した
（2026-11-14。設定画面の「入力」タブ。既定 OFF・試作）。認識は SAPI（Windows の音声認識）の日本語の認識エンジン
（Microsoft Speech Recognizer 8.0 for Windows (Japanese)）で、端末内で動く（ユーザの判断。Windows の「オンライン音声認識」・
「強化された音声認識」は使わない）。録音と認識は変換サーバの中で行う（`shared/voice_recognizer.cpp`）

- キーは右 Ctrl（既定）か右 Shift（`config.json` の `voiceInputHoldKey` が `rightControl` / `rightShift`。macOS版は右 ⌘ / 右 Option）。
  右 Alt は単独で押して離すとアプリのメニューに移るので選べないようにした。TIP は押した・離したを変換サーバに送るだけで、
  キー自体はアプリにも渡す（右 Ctrl+C などはそのまま効く）。押してから 0.12 秒待ってマイクを開き、その前に離した・
  ほかのキーが来た（ショートカット）・押していたのが 0.35 秒未満なら捨てる（macOS版と同じ）。離したことを取りこぼしたときに
  備えて、問い合わせのタイマーでキーの状態も読む
- マイクは waveIn（16kHz・モノラル・16 ビット）で受け、書き足されるまで読み取りを待たせるストリームで SAPI に実時間で流す。
  話している間は途中結果（仮説）が約 0.25 秒ごとに届き、未確定文字列の末尾に足して見せる（macOS版と同じ。文節変換中は小窓に出す）。
  離してから結果が届くまで約 0.5 秒（読み上げ音声で実測）
- 読みは、結果の語ごとの表示形（数字への置き換えの前。「三人」）と語彙形（「さんにん」）から付ける
  （`VoiceTranscript::TokenFromRecognition`: 表示形がかなならそれ、漢字とかなだけなら語彙形、英字・数字を含めば読みなし）。
  語彙形は発音寄りで、助詞の「は」が「わ」になるため、かなの語は表示形を使う。読みに分ける規則・数字と助数詞の扱い・
  句読点・音声認識との違いの知らせ（`VoiceComparison`）は macOS版の `VoiceTranscript` / `VoiceComparison` と同じ（テストは 1 対 1）
- 入れたあとの扱いは macOS版と同じ: 確定はしない。固定部分（英字・数字）の前の読みはそこで変換して固定する。
  声で入れた読みを含む合成は、学習・変換記録・入力履歴に残さず、打ち間違いの訂正にも通さない。合成が空のところへ入れたときは、
  ライブ変換が届いたら音声認識の表記と違う箇所を小窓に出す。Windows版では、ライブ変換が届くまでの間は読み（かな）ではなく
  認識結果の表記を見せる（漢字 → かな → 漢字と表示が揺れないように）
- 合成が無いところで押したときも、TIP は録音を始めた文書に向けて問い合わせを続け、結果を非同期のエディットセッションで入れる
- 日本語の認識エンジンが無い機械では、押したときに「こえ: 日本語の音声認識エンジンが見つかりません」と出す。マイクの使用は
  Windows の設定（プライバシーとセキュリティ > マイク > デスクトップ アプリにマイクへのアクセスを許可する）に従う
- **実際のマイクと TIP のキーの扱いは、まだ確かめていない**（この機械の利用者の声を録らないため）。読み上げ音声を
  マイクの代わりに流して、変換サーバから先（途中結果・結果・短く押した・ほかのキー・打った読みに続ける）を確かめた

```powershell
# 読み上げ音声を作って認識する（--stream はマイクと同じく実時間の速さで流し、途中結果の届く時刻も出す）
windows\build\iroha-cli\iroha-cli.exe voice --stream --say "東京駅で資料を受け取ってから、山田さんと三人で向かいます。" voice.wav
# マイクから 5 秒録って認識する（変換サーバと同じ経路）
windows\build\iroha-cli\iroha-cli.exe voice --mic 5
# 変換サーバを IROHA_VOICE_FILE=voice.wav で起動すると、マイクの代わりにそれを流す。<vstart>・<vstop> がキーを押す・離す
windows\build\iroha-cli\iroha-cli.exe session "<vstart><pause5600><vstop><wait><enter>"
```

## 打ち間違いの訂正

macOS版の `TypoNormalizer`（ローマ字入力の打ち間違いを、かな漢字変換の手前で「読み → 読み」で直す文字単位の
Transformer）を移した（2026-11-13）。既定 OFF。設定画面の「入力」タブで ON にしたときにモデル（標準は iroha-typo-v2、
約 23MB、CC BY-SA 4.0）を取得する。カタログ（`models/typo-normalizer-format2.json`）・置き場所
（`%LOCALAPPDATA%\iroha\models\typo-normalizer\`）・`installed.json` の形は macOS版と同じ

- `iroha-core` の `typo_normalizer`: 重みの展開（float32・float16・q6）・エンコーダ／デコーダ・数字の扱い（`NumberMask`）・
  文節への割り付け・小窓の割り付け。式は macOS版と同じ順で float32。行列の積だけは内積を AVX2（無ければ SSE2）で
  まとめて足す。1 回の訂正は読み 10〜20 文字で 15〜60ms（iroha-typo-v2、このWindows機の CPU 1 スレッド）。
  試験用の iroha-typo-v3（q6、46.2M）は 100〜180ms
- 入力の流れは macOS版と同じ: 入力が止まって 500ms たったら読みを直し（`InputSession::ScheduleTypoCorrection`。
  休止の判定と推論は変換サーバの裏のスレッドで、変換エンジンとは別に行う）、「打った読み → 直した読み」を
  未確定文字列の下に出す。直した直後の Backspace で取り消し、取り消した読みは再び直さない。Space で文節変換したときは
  読みを書き換えず、訂正を候補ウィンドウの 2 番目（変換ルールの後ろ）に出す（休止で同じ読みを確かめてあれば
  その結果を使う）。差分が文節境界をまたぐときは、文全体を訂正した候補を出し、選んだら文節ごと置き換える。
  末尾に足すだけの訂正は捨てる。訂正の候補を選んだ確定は学習しない
- しきい値・待ち時間・最低文字数は `config.json` の `typoNormalizerThreshold`（2.5）・`typoNormalizerDelayMs`（500）・
  `typoNormalizerMinLength`（4）。キーの名前は macOS版の設定と同じ
- 照合: 書き出しに付いてくる parity.json があれば `iroha-cli typo parity --dir <書き出し>`（macOS版と同じ判定）。
  2026-11-13 は parity.json が手元に無いので、numpy で同じモデルを組んでロジットを突き合わせた（iroha-typo-v2・v3 とも
  最大差 4×10⁻⁶、macOS版の基準は 10⁻³）。macOS版のテストの期待値（「3じにかくいんしました」→「3じにかくにんしました」など）も
  単体テストに入れてある（`IROHA_TYPO_MODEL` でモデルのフォルダを指したときだけ回る）

```powershell
# モデルの一覧と取得（設定画面と同じ経路）。訂正を試す（読みはかなのまま渡す）
windows\build\iroha-cli\iroha-cli.exe typo catalog install
windows\build\iroha-cli\iroha-cli.exe typo --threshold 2.0 をわぇてけいやくする とうじょうsじない
# 変換サーバを通して、続けて打ったあとの休止での訂正と Backspace での取り消しを見る（<wait> で結果を待つ）
windows\build\iroha-cli\iroha-cli.exe session --interval 90 "kakuinshimashita<wait><bs><wait>"
```

## 記号・顔文字・絵文字の候補

文節の候補ウィンドウに Mozc（BSD 3-Clause、Google）の記号・顔文字・絵文字を足す（2026-11-13）。
規則は macOS版の `SymbolCandidateTable` / `SymbolCandidateMerger` と同じで、`iroha-core` の `symbol_candidates` に移した
（テストは 1 対 1）。候補の並びは「今の表示 → エンジンの候補（絵文字は上から 7 番目）→ 顔文字 → 記号 → ひらがな・カタカナ」。
ライブ変換には使わず、表にある候補（漢字・かなだけのものを除く）を選んだ確定は学習しない。
「えもじ」で全部の絵文字、「かおもじ」で全部の顔文字。`config.json` の `"symbolCandidates": false` で止められる

- データは `fetch-symbols.ps1` が macOS版と同じコミットで `vendor/mozc-data` に取り、`install-tip.ps1`・配布 zip が
  実行ファイルと同じフォルダの `Symbols` に LICENSE と一緒に置く
- Windows で描けない絵文字は、変換サーバが読み込むときに除く（`shared/emoji_renderability.cpp`）。規則は macOS版の
  `EmojiRenderability` と同じ（字形がすべて Segoe UI Emoji にあり、部品より少ない字形で描かれる）に、もう 1 つ足した:
  字形が 1 つでも先頭の部品だけと同じ字形なら除く（イングランドの旗などのタグ文字の旗は、Windows ではタグを捨てて
  黒い旗 🏴 になる）。2026-11-13（Windows 11 26200）: 全 1,918 個のうち 294 個を除いた（国旗・Unicode 16 の絵文字など）。
  読み込みは 62ms
- 候補ウィンドウは Direct2D・DirectWrite で描く（GDI では絵文字が白黒になり、肌の色の組み合わせなども 1 つの絵に
  ならない）。絵文字の候補は Segoe UI Emoji で描く（Meiryo UI にも ❤ などの白黒の字形があるため）。
  Direct2D を作れなければ GDI で描く

```powershell
# 表を引く（Windows で描けないとして除いた絵文字の数と例も出す。リポジトリの下で実行する）
windows\build\iroha-cli\iroha-cli.exe symbols neko warai kaomoji
```

## 専門辞書

配布している辞書（JMdict の分野別の用語・百薬辞書・郵便番号データの地名・Wikidata の人名や駅など）を、
設定画面の「専門辞書」タブから 1 つずつ入れ外しする（2026-11-13）。カタログ（`models/term-dictionaries.json`）・
辞書ファイル・置き場所の形式（`%LOCALAPPDATA%\iroha\dictionaries\<ID>.tsv` と `<ID>.json`）は macOS版と同じ。
使い道は候補ウィンドウだけで、ライブ変換の第一候補は変えない（macOS版と同じ）

- `iroha-core` の `term_dictionary`: カタログ・設置の記録・辞書名の印（`TermDictionaryLookup`。辞書ファイルを
  読み・語の順の二分探索で引く）。テストは macOS版の `TermDictionaryTests` を移したもの
- 辞書ラティスへの加え方は macOS版と違う: macOS版は azooKey のユーザ辞書の形式（LOUDS）に書き出して読ませるが、
  Windows版は同じたどり方ができる木をメモリに作り（`src/lattice/term_index.cpp`）、本体の辞書と並べてたどる
  （どちらかに続きがある限り読みを伸ばす。azooKey がユーザ辞書「user」を扱うのと同じ）。品詞・評価値（-10）・
  1 文字の読みを入れないこと・同じ組を最初の辞書だけ残すことは macOS版と同じ。ユーザ辞書の語の境目の判定には入れない。
  本体の辞書とユーザ辞書の語の並ぶ順は azooKey ではハッシュ表の順で決まらないので、専門辞書を入れたときの
  候補の並びは macOS版と照合していない
- 取得（`shared/term_dictionary_fetcher.cpp`）は URLDownloadToFile で落とし、大きさと SHA-256（BCrypt）を照合してから置く。
  設定画面は「専門辞書」タブを初めて開いたときに一覧を取り、作り直された辞書を入れ直す（macOS版と同じ）。
  入れ外しのあとは変換サーバに Reload を送り、変換サーバは入れてある辞書の組（ID と SHA-256）が変わっていれば読み直す
- 候補ウィンドウは、専門辞書の語（か、その語＋読みの残りのかな）になっている候補の右に辞書の名前を小さく出す。
  通信では候補の後ろに印の列を足した（足す前の返事も読める）
- 確認（2026-11-13）: 医学・工学・人名・地名（計 30 万語）で、ラティスに読ませるのに約 1 秒。人名と地名から
  抜き出した語は、本体の辞書に無いものも専門辞書を入れたときだけ候補に出た。SHA-256 の合わないファイルは置かない

```powershell
# 一覧・取得・削除（設定画面と同じ経路）。辞書名の印を引く
windows\build\iroha-cli\iroha-cli.exe terms list
windows\build\iroha-cli\iroha-cli.exe terms install jmdict-medicine
windows\build\iroha-cli\iroha-cli.exe terms lookup goenseihaien 誤嚥性肺炎
# 入れた辞書を候補に加えて変換する（既定は使わない。macOS版と同じ）
$env:IROHA_TERM_DICTIONARIES = "installed"
windows\build\iroha-cli\iroha-cli.exe lattice goenseihaien
```

## 言語バーのメニュー

タスクバーの「あ」「A」をクリックするとメニューを開く（2026-11-14 に、クリックで切り替え・右クリックで設定画面、から変えた。
ユーザの指定で Windows の IME と同じ形）。ボタンは `TF_LBI_STYLE_BTN_MENU` で、メニューはシステムが出し、項目は
`LangBarButton::InitMenu` が `TextService::MenuEntries` から渡す（`OnClick` が呼ばれたときは同じ項目を自分で出す）。
項目は macOS版の入力メニューに合わせた: ひらがな／半角英数（Windows の IME と同じ入力モードの切り替え）・設定...・
ユーザ辞書...・変換ルール...・プライベートモード・ライブ変換・予測変換・句読点スタイル・モデルフォルダを開く・
アップデートを確認...・版。macOS版の「原稿に合わせる」（Windows に無い機能）と変換モデルの取得状況は出していない。
ライブ変換・予測変換・プライベートモード・句読点の切り替えは変換サーバへキー（`ToggleLiveConversion` など）として送り、
変換サーバが設定に保存してほかのセッションにも効かせる。印に使う今の設定は返事の flags で受け取る

## デバッグ表示

macOS版の開発者モードの「推論にかかった時間と左文脈をカーソルの右下に表示する」（`DeveloperOverlay`）を移した
（2026-11-14。設定 > 情報 > 開発者向け の「デバッグ表示」、`developerOverlay`。開発者モードが ON のときだけ効く。既定 OFF）。
Linux版（f69ceab の段階で `linux/iroha-core` に入ったもの）と同じ作りで、コアの部分はそこから写した。

- 行は 3 本: 入力を始めたときの左文脈（アプリから読めたか・何文字か。読めなければ代わりの確定文字列）、かな漢字変換 1 回ごと
  （ライブ変換・文節に分ける・区切り直し・候補ウィンドウ。全体の時間とそのうち NN の時間と回数、モデルの読み込み、結果を
  使わずに打ち切った変換の数）、打ち間違いの訂正 1 回ごと。文面は macOS版と同じ（`iroha-core` の `debug_overlay`）。
  「要素の文字」の行と加速度センサのグラフは macOS 専用なので無い
- NN の時間は `inference_timer`（スレッドごとの入れ物）に `ZenzEngine`・`TypoNormalizer` が足し込む。学習・ユーザ辞書・
  区切りのキャッシュで NN を通さなかったときは理由を添える。ライブ変換と休止での訂正は `SessionManager` の裏のスレッドで測る
- 変換サーバが `SessionOutput::debugOverlay`（改行区切り）で返し、TIP が未確定文字列の**上**に出す（候補・予測・知らせの
  小窓は下に出るので重ねない）。Enter で確定したとき・合成が終わったときに閉じる。`iroha-cli session` も行を出す

## 配布（インストーラーと自動アップデート）

2026-11-14 に作った。配布物は `iroha-<版>-windows-x64-setup.exe` だけ（zip は置かない。macOS版のアップデータは
「iroha- で始まり .zip で終わる」ファイルを取るので、Windows の zip を同じ Release に置くと取り違える）。

- 作り方: `windows\scripts\make-installer.ps1 [-Version 0.18.0]`（ビルドとテストのあと、一式を %TEMP% に集めて
  Inno Setup 6 の `windows\installer\iroha.iss` でまとめる。出力は `windows\build\installer\`）。Inno Setup は
  `winget install JRSoftware.InnoSetup`。リリースは `.github/workflows/release.yml` の Windows のジョブがタグの版で作る
- インストーラーがすること: 変換サーバ・設定画面を止め、読み込まれている `iroha-tip.dll` は名前を変えて退避してから
  新しいものを置いて TSF に登録（使っているアプリを閉じる仕組みは使わない。TIP はすべてのアプリに読み込まれているため）、
  辞書・記号のデータ・ライセンスの全文を置き、変換サーバのログオン時の起動を HKLM の Run に登録（以前の手作業の
  HKCU の登録は消す）、変換モデルが無ければ取得（大きさと SHA-256 は macOS版の `ModelDownloader.swift` と同じ）、
  変換サーバを普段のユーザの権限で起動する。スタートメニューに「iroha の設定」、「インストールされているアプリ」に
  アンインストールの項目を作る。アンインストールしても `%LOCALAPPDATA%\iroha`（学習・辞書・設定・モデル）は残す
- `AppId`（iroha.iss）は変えない（変えると別のアプリとして入り、アップデートで上書きされなくなる）
- 自動アップデート（macOS版 `UpdateChecker` と同じ考え方）: 変換サーバが 1 時間ごとに確認の時期（前の確認から 24 時間）かを
  見て、時期なら `iroha-settings.exe --check-update` を起動する。設定画面が GitHub の `releases/latest`（プレリリースと
  下書きは返らない）を読み、新しい版があれば知らせる（アップデート／リリースノート／閉じる、自動のときは「この版を
  スキップ」）。「アップデート」でインストーラーを落とし、大きさと SHA-256（GitHub の asset の `digest`）を照合してから
  `/SILENT` で起動する。インストーラーは自分で管理者の許可を求める（設定画面から昇格させると、変換サーバを普段のユーザで
  起動し直せない）。確認した記録は `%LOCALAPPDATA%\iroha\update.json`（設定画面が書き、変換サーバは読むだけ）。
  手動の確認と自動の確認の ON/OFF は 設定 > 情報 > アップデート（`autoUpdateCheck`、既定 ON）。
  確認先は `updateCheckURL` で差し替えられる（プレリリースで試すとき。`file:///` も読める）
- 版: `windows/CMakeLists.txt` が `IROHA_VERSION`（無ければ `git describe --match "v[0-9]*"`）を各実行ファイルの
  VERSIONINFO と `IROHA_VERSION` の定義に入れる（`shared/version.rc.in`）。開発版の「0.17.1-29-gcf75edd」は、
  アップデートの確認では 0.17.1 として比べる（`Update::ComparableVersion`）
- アイコン: `windows/resources/iroha.ico`（`windows/scripts/make-icon.py` が macOS版の AppIcon.icns から余白を詰めて作る）。
  TIP・設定画面・変換サーバ・インストーラーが同じものを使う
- 署名はしていない（コード署名の証明書が無い）。ブラウザで落としたインストーラーは SmartScreen が「不明な発行元」として
  止めることがある。アップデートで落としたものは SHA-256 を照合している（GitHub のアカウントが乗っ取られた場合は防げない）

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
      言語バーの入力モードボタン「あ/A」・テーマ連動アイコン。2026-11-14 からクリックでメニュー）
- [x] M6a: 設定ウィンドウ（iroha-settings.exe: モデル選択・ダウンロード・切替、
      ユーザ辞書の一覧/追加/削除、学習リセット。あ/Aボタンのメニューの「設定...」で起動。
      config.json + IPCのReloadでサーバへ即時反映）
- [x] M7a: 辞書ラティスのC++移植（`iroha-core/src/lattice/`。macOS版の候補の並びと一致を確認）
- [x] M7b: モデルの採点・T5（iroha-kkc-v1）対応と、辞書ラティスの候補の再採点（`LatticeRescoringEngine`）。
      変換サーバに組み込み、既定のモデルを iroha-kkc-v1 に変更。候補ウィンドウのページ送り
- [x] M7c: 長い読みの分割（ChunkedConversionEngine）と異体字（VariantKanjiEngine）
- [x] M7d: 学習・ユーザ辞書を macOS版の今の仕様に合わせる（直す前の結果つきの学習、ユーザ辞書の語の境目、
      候補ウィンドウ専用の語）
- [x] M7f: ライブ変換。入力の状態を変換サーバ側に移した（`InputSession` / `SessionManager`。
      TIP はキーを送って表示を反映するだけ）
- [x] M7e-1: 記号・顔文字・絵文字の候補（Mozc のデータ、Windows で描けない絵文字を除く、候補ウィンドウを Direct2D で描く）
- [x] M7e-2: 専門辞書（設定画面のタブ・辞書ラティスへの組み込み・候補の辞書名の印）
- [x] M7m: 設定画面の作り直し（Windows 11 の設定アプリの見た目・「?」の説明・開発者モード）、
      選択テキストのマウスのきっかけと文字数の表示
- [x] M7l: 「こえ」モード（SAPI の日本語の認識エンジン・右 Ctrl を押している間）
- [x] M7k: 選択テキストのAI編集（クリップボード経由・設定画面の「選択テキスト」タブ）
- [x] M7j: 候補ウィンドウを表に広げる（Tab）・区切り記号で囲んだ英字入力・AIで直す
- [x] M7i: AI変換（Ollama・LM Studio・OpenAI互換・Claude、プリセット 3 つ、設定画面の「AI」タブ）
- [x] M7h: 打ち間違いの訂正（TypoNormalizer の C++ 移植・休止での訂正と取り消し・文節の訂正候補・設定画面からの取得）
- [x] M7g: macOS版のキーの扱い（表示形・英字入力・文節の打ち直し・確定の取り消し・句読点の切り替え）、
      予測変換・変換ルール・プライベートモード・変換記録と、設定画面の「入力」「変換ルール」「学習・記録」タブ
- [ ] M5b: UIlessモード（ITfCandidateListUIElement。タスクバー検索ボックス等）
- [x] M6b: インストーラー（Inno Setup）・自動アップデート・release.yml の Windows のジョブ・アイコンと版の情報
- [ ] M6c: コード署名（証明書が要る）

参考実装: CorvusSKK（構成の手本）、Microsoft textservice step01-06（チュートリアル）、
SampleIME（登録・UIlessの正解集）、mozc `src/win32/tip/`（完成形）。
