# iroha 開発ガイド（Claude Code用）
まず、報告、会話は日本でお願いします。また、報告内容で気の利かせた言い回しや比喩は避けてください。名前に関しても省略名称は極力使わないか、使う場合は事前にこのように呼ぶ、報告を先にしてください。

## リリースポリシー（重要）

**ユーザーが明示的に「リリースして」と言うまでリリースしない。**
機能追加・修正はコミット + push + ローカルインストール（`./macos/scripts/install.sh`）まで。
`git tag vX.Y.Z && git push origin vX.Y.Z` はリリース指示があったときのみ実行する
（タグpushでGitHub Actionsが署名・公証・Release作成まで自動実行される）。

### 両プラットフォームのリリース方針（決定済み・Windows初回リリース時に実装）

- バージョンは macOS / Windows で**単一の系列（`vX.Y.Z`タグ）を共有**する。
  プラットフォーム別タグ（`macos-v…` 等）は作らない
- 1つのタグpushで両OSをビルドし、**1つのGitHub Releaseに両方の成果物を添付**する
  （`iroha-X.Y.Z-macos.zip` / `iroha-X.Y.Z-windows.zip` のようにアセット名にOSを入れる）
- release.ymlは「draft作成 → 各OSジョブがアップロード → 全部揃ったらpublish」の順にする
  （片方だけ公開されるとアップデータが自分のOSのアセットを見つけられないため）
- **注意**: アセット名を `iroha-X.Y.Z.zip` から変更する際は、配布済みmacOS版の
  `UpdateChecker.swift` が旧名を前提にしていないか先に確認する（壊れる場合は
  新旧両名でアップロードする移行リリースを挟む）
- `-` を含むタグ（例 `v0.6.0-beta.1`）はプレリリース＝アップデート通知対象外。
  Windows版が未成熟な間のベータ配布はこれを使う

## 日常の開発コマンド

```sh
cd macos && swift build --build-system native && swift test --build-system native
                                        # ビルドと単体テスト（必ず macos/ から実行。
                                        # Package.swiftのvendor/dist参照がcwd相対のため）
./macos/scripts/install.sh       # ローカルの ~/Library/Input Methods/ へインストール（ad-hoc署名）
/usr/bin/log stream --predicate 'process == "iroha"' --style compact  # IMEログ

# 設定ウィンドウだけを開く（IMEに接続しない。UI確認用）
"$HOME/Library/Input Methods/iroha.app/Contents/MacOS/iroha" --settings [input|dictionary|selection|model|about]
```

- `swift build` / `swift test` には `--build-system native` を付ける。Xcode 27 の Swift は既定が新しい仕組み
  （swiftbuild）で、mlx-swift の .metal のコンパイルで失敗する。ビルド先も変わり、スクリプトは native の
  `.build/<config>/` を前提にしている（2026-10-24。native は廃止予定と警告が出る）。
  Xcode の版を上げると Metal Toolchain も入れ直しになる（`xcodebuild -downloadComponent MetalToolchain`。
  無いと install.sh が `build-mlx-metallib.sh` で止まる）。
  `macos/.build` は Dropbox の同期から外してある（`xattr -w com.dropbox.ignored 1 macos/.build`、Mac ごとに要る。
  版の違う Xcode の 2 台が同じ `.build` を書き合っていた）
- `install.sh` はキーチェーンに Developer ID Application の証明書があればそれで署名する（無い Mac では ad-hoc。
  公証しないので署名の時刻は付けない）。ad-hoc の識別条件はビルドごとに変わる cdhash で、更新のたびに API キーの
  キーチェーンのダイアログが出ていた（2026-10-27）
- **キーチェーン（API キー）を main thread で読まない**（`SecretStore`。`get` は裏のスレッド用、main thread は
  `cachedValue` / `load`）。設定画面は IME と同じプロセスにあり、読み取りがダイアログを出すと答えるまで main thread が
  止まり、どのアプリの入力にも応えられなくなる。パスワードのダイアログ自体も iroha を入力メソッドとして待つので、
  答えるまで数分かかった（2026-10-27 実測 4 分 19 秒）
- `log` はzshの組み込みと衝突するため必ず `/usr/bin/log` をフルパスで呼ぶ
- NSLogはこの環境ではユニファイドログに残らない。IMEの実行時調査は
  /tmpへのファイル直書きヘルパーを一時的に仕込む（調査後に削除）
- 起動・終了・異常終了の履歴は `<データフォルダ>/logs/launch-<ホスト名>.log`（`LaunchLog`）。
  予期しない再起動やクラッシュの調査はまずここと `~/Library/Logs/DiagnosticReports/iroha-*.ips` を見る
- **`~/Library/Input Methods/` の中に iroha.app 以外の .app（仮置き・旧バンドル）を置かない。**macOS は
  このフォルダを監視して入力ソースの情報を作り直すため、同じバンドルIDのものが並ぶと入力メニューから
  iroha が消え、ログアウトまで戻らない（2026-09-26 実測。install.sh と `SelfInstaller.replaceInstalledBundle` の
  コメント参照）。入れ替えの数秒後に `iroha --refresh-input-menu` で入力メニューに一覧を読み直させる。
  `killall TextInputMenuAgent` 等では直らない
- **入力メニューで iroha が灰色になるのは、別のアプリが Secure Input を持ったままのとき**（2026-10-30〜31 実測。
  iroha の不具合ではない）。持ち主は `ioreg -l -w 0 | grep kCGSSessionSecureInputPID`。Chrome はパスワードの画面を
  離れても解除せず、終了するまで続いた。灰色は Secure Input が有効になったあと**前面のアプリを切り替えたとき**に出て、
  解除されても次に切り替えるまで残る（確かめるときは必ず Finder などに切り替えてから入力メニューを見る）。
  azooKey は選べるが、置き場所（`/Library`）・App Sandbox・プロビジョニングプロファイル・公証・Info.plist の接続名・
  入力メソッドとしての動き・ほかのアプリの見張りのどれを azooKey に合わせても iroha は灰色のままで、条件は分からなかった。
  そこで知らせる: `SecureInputNotice`（判定は IrohaCore の `SecureInputNoticeRule`、
  設定 > 入力 > 入力メニュー、既定ON・同期する）が、前面のアプリが切り替わった 2 秒後に確かめ、持ち主が前面のアプリでも
  iroha 自身（設定画面の API キー欄）でもなければ、アプリ名と戻し方をマウスポインタの近くに 8 秒出す（`TapInputFeedback`）。
  同じ持ち主には解除されるまで 1 回だけ。前面のアプリ自身が持ち続ける場合（Terminal の「セキュアキーボード入力」）は
  パスワード欄と見分けられないので知らせない。再現は前面に出ないプロセスで `EnableSecureEventInput()` を保持して作る
  （そのときの持ち主は、そのプロセスを起動したアプリの PID で記録される）

## プロジェクト構成の要点

ディレクトリは役割で分ける（README.md の「ディレクトリ構成」節と同じ）。**新しいものを置くときはこの表の役割で場所を決める**:

| 役割 | ディレクトリ | Git |
|---|---|---|
| アプリ | `macos/`（Swift パッケージ）・`windows/`（TSF） | 追跡 |
| 配布・評価 | `models/`（アプリが取得するモデルの一覧）・`testdata/`（評価セット。`ajimee/` は追跡外） | 追跡 |
| 学習データを作る | `dataset/iroha-typo-normalizer/`（コードは `dataset/iroha-typo-normalizer/src/iroha/`、`python -m iroha …`。作ったデータは `dataset/iroha-typo-normalizer/data/` で追跡外） | コードだけ追跡 |
| 学習 | `training/`（直下がかな漢字変換モデル、`typo-normalizer/` が打ち間違いの訂正） | 追跡外 |
| アプリに入れない検証 | `experiments/`（`reranker/`・`jev/`） | 追跡外 |
| 外部依存 | `vendor/`（llama.cpp・辞書データ）・`patches/` | パッチだけ追跡 |
| 非公開の文書 | `grants/`・`論文/` | 追跡外。**触らない** |

- 学習データの置き場所は `dataset/iroha-typo-normalizer/data/`、学習とその記録は `training/`、アプリに載せたモデルの
  評価セットのうち追跡するものは `testdata/`。同じ取り組みの物がこの 3 か所に分かれるのは役割の違いによる
  （例: 打ち間違いの訂正は、読み一覧 `dataset/iroha-typo-normalizer/data/typo-corpus/`・学習 `training/typo-normalizer/`・
  書き下ろしのベンチ `testdata/iroha/typo/`）
- `dataset/iroha-typo-normalizer/` は 2026-09-24 に `iroha-dataset/`（パッケージ `iroha_dataset/`）から移した。`dataset/iroha-typo-normalizer/` の下には
  データセットごとに `dataset/iroha-typo-normalizer/<名前>/` を置く。`.venv` の編集可能インストールはパスを持つので、移したあとの別マシンでは
  `cd dataset/iroha-typo-normalizer/iroha && ./.venv/bin/python -m pip install --no-deps -e .` で入れ直す。
  pip のパッケージ名・`--version`・出典表記の「iroha-dataset」は名前として残している
- **保留中の整理**（2026-09-24 決定）: `training/` 直下のかな漢字変換モデルの学習一式（llm-jp・T5）を
  `training/kkc/` へ移す。GPU マシンで T5 の本番学習（`training/t5/iroha-t5-e12d2-full`）が
  進んでいる間は動かさない（Dropbox の同期でパスが変わり学習が壊れる）。学習が終わってから、
  スクリプトの相対パス（`../vendor` → `../../vendor` など）と CLAUDE.md・README の参照を合わせて移す
- プラットフォーム別レイアウト: macOS版のSwiftパッケージ一式（Package.swift / Sources /
  Tests / Resources / scripts）は `macos/` 配下。Windows版は今後 `windows/` に実装する。
  `vendor/`（llama.cpp）・`patches/`・`testdata/`・`training/`・`.venv` はプラットフォーム共有の
  ためリポジトリ直下に置く（**`training/` のスクリプトがルート直下の `vendor/`・`.venv` を
  参照しているので、これらを `macos/` 配下へ移動してはならない**）
- **`training/` と `experiments/` はリポジトリに入っていない**（2026-09-22 に追跡から外した。
  他者のデータを iroha のリポジトリから再配布しないため。`.gitignore` の該当行に理由がある）。
  中身が要るときは `git checkout v0.12.0 -- training experiments` で履歴から取り出す。
  - このMacにも `training/` の全体（`t5/`・`iroha-llmjp-150m-full/` などのサブディレクトリを含む、
    2026-09-24 時点で 79GB）が Dropbox で同期されている。ただし T5 側の作業や学習そのものは
    GPU マシン（`/data1/Dropbox/project/iroha`）で行い、このMacからは起動しない。
    `typo-normalizer/` だけはこのMac（MPS）で学習する
  - `experiments/` はこのMacに全部ある（`reranker/` と `jev/`）。追跡していないだけ
- `macos/Sources/iroha/` — IME本体（Swift 5モード）: IMKコントローラ、設定UI、
  AIバックエンド（Apple FoundationModels / Ollama / LM Studio / OpenAI互換 / Claude。Claude は Messages API を直接呼び、
  応答の読み取りは `ClaudeStreamReader`。既定モデル claude-haiku-5-5）、
  アップデータ、モデルDL、macOSユーザ辞書の取り込み（`SystemUserDictionary`）、
  選択テキストのAI編集（`Selection/`。グローバルショートカットとマウス選択で
  他アプリの選択テキストをAIで置換する。GenGoの機能を移植。既定OFF・要アクセシビリティ権限）
- `macos/Sources/IrohaCore/` — 変換エンジン（Swift 6モード）: zenz + llama.cpp。
  Foundationのみ依存でmacOS専用APIは不使用（将来のWindows移植候補）
- 設定画面（`SettingsView.swift`）の項目の細かな説明は画面に並べず、「?」ボタン（`SettingsHelp.swift` の
  `HelpButton` / `HelpLabel` / `HelpToggle` / `HelpSectionHeader`）に入れ、押したときにポップオーバーで出す
  （2026-09-30）。状態・警告・エラーの表示は「?」に入れず画面に出す。無効にするのは操作部品だけにして、
  機能が OFF でも説明は読めるようにする
- 設定画面には一般ユーザ向けと開発者モード向けの 2 段がある（2026-10-03。`DeveloperModeSettings`、
  設定 > 情報 > 開発者モード、既定 OFF・同期しない）。細かな調整・開発者向けの項目は `if developerMode` の中に置き、
  一般ユーザの画面には出さない。隠すのは表示だけで値はそのまま効く（例外はデバッグ表示で、開発者モードが OFF なら止める）。
  **新しい設定項目を足すときは、どちらに置くかを決めてから足す**。開発者モードの項目一覧は情報タブの「?」の文面にもあるので合わせる。
  開発者モードの項目には紫色の「開発者」の印を付ける（`DeveloperBadge`。項目は `HelpLabel` / `HelpToggle` の `developer: true`、
  節ごとなら `HelpSectionHeader` の `developer: true`。2026-11-07）。**`if developerMode` の中に項目を足したら印も付ける**
- FoundationModelsはmacOS 26+のため `#if canImport` + `@available(macOS 26.0, *)` ガード必須
  （パッケージのフロアはmacOS 14）
- llama.cppの静的ライブラリは `./macos/scripts/build-llama.sh` で `vendor/dist` に生成（未コミット）
- 候補ウィンドウの候補は辞書ラティス（SwiftPM依存 AzooKeyKanaKanjiConverter、MIT）で作り
  zenzの対数確率で並べる（`LatticeRescoringEngine`）。辞書データ（Apache-2.0）は
  `./macos/scripts/fetch-dictionary.sh` で `vendor/azooKey_dictionary_storage` に取得（未コミット、
  コミット固定）し、`make-bundle.sh` が `Contents/Resources/Dictionary` へコピーする。
  AzooKeyKanaKanjiConverterのバージョンを上げるときは、そのタグがサブモジュールで参照する
  辞書コミットに `fetch-dictionary.sh` の `DICT_COMMIT` を合わせる。
  Zenzaiトレイトは使わない（同梱のllama.cpp xcframeworkが `vendor/dist` と衝突するため）。
  ライブ変換の第一候補はzenz生成のまま（AJIMEE: 生成84.5% vs ラティス再採点66.5%）。
  候補ウィンドウは「採点した上位」＋「読みが一致する残りの辞書エントリ」＋「内蔵表の異体字
  （`VariantKanjiEngine`、髙・﨑など）」の順で、候補数の設定より多く返る
- ユーザ辞書・学習はLLMの外側で処理する（`ConversionEngine`のデコレータを
  学習 → ユーザ辞書 → 長い読みの区切り（`ChunkedConversionEngine`、80文字超を
  句読点/文節境界で分割して逐次変換）→ LLM の順に重ねる。ユーザ辞書は読みを分割して
  一致部分を埋め、学習は読み全体が一致したときだけ差し替える）。
  ユーザ辞書の読みの途中の一致は、辞書ラティスにその語を固有名詞として入れて変換したときの語の境目に
  始まりと終わりがある所だけ使う（`LatticeConverter.wordBoundaries`。2026-10-25。読みの並びだけで当てると
  「えま→江間」が「きえました」に当たり「来江間した」になった）。
  macOS側の辞書は読み取り専用で絶対に書き込まない。
  ユーザ辞書のうち単語が読みより大幅に長いエントリ（`UserDictionary.isSuitableForLiveConversion`
  が偽: 2倍超かつ3文字以上長い。ハッシュタグ・URL・定型文など）はライブ変換・文節分割には
  使わず候補ウィンドウにだけ出す（ことえりと同じ体感）。この語を候補から確定しても学習しない
  （学習は辞書の外側にあるので、覚えるとライブ変換に戻ってくる）
- 打ち間違いの訂正（`TypoNormalizer`、`macos/Sources/IrohaCore/TypoNormalizer/`、既定OFF）は
  かな漢字変換の**手前**で「読み → 読み」を直す 3.2M の文字単位 Transformer（実装は Accelerate の
  `cblas_sgemm` だけ。llama.cpp も MLX も通さない）。学習・評価の環境と実験の記録は
  `training/typo-normalizer/`（2026-09-24 に `experiments/` から移した。リポジトリには入っていない。
  学習データの読み一覧は `dataset/iroha-typo-normalizer/data/typo-corpus/`（書き言葉の readings-balanced / full と、
  話し言葉の readings-spoken。話し言葉は `config/typo-spoken.yaml`・LICENSES.md H 節。open2ch は掲示板のクロールで、
  除外条件の例外として 2026-09-25 にユーザの判断で入れた）。実験の経緯と数字は同ディレクトリの README.md）、
  **移植が正しいかの判定は `iroha-cli typo parity`（PyTorch 実装との照合 200 件）で行う。**
  `training/typo-normalizer/SWIFT-PORT.md` は移植を頼むときに書いた開発機間の伝言メモで、
  実装が進んだ今は内容が古い。ソース中の `training/typo-normalizer/…` への参照も出自を示すもので、
  clone には含まれない（読みたいときは作業機か `git checkout v0.12.0 -- experiments`。
  このときの置き場所は旧パスの `experiments/typo-normalizer/`）。守ること:
  ・**計算の順序を変えたら `iroha-cli typo parity` を必ず回す**（PyTorch 実装との照合 200 件。
    生成が一致してもロジットがずれていれば実装は間違っている。float32 でロジット 1e-3・logP 0.01 以内）
  ・**本線は「入力の休止」で読みそのものを直す**（`scheduleTypoCorrection`、既定 500ms・設定可。2026-11-06 に 300ms から変更）。
    iroha はライブ変換が主でスペースを押さずに確定することも多く、しかも打ち間違いに
    気づいた人はスペースではなく Backspace を押すので、変換要求を起点にすると遅い。
    直した直後の Backspace だけは「1文字消す」ではなく訂正の取り消しに使う
    （`undoTypoCorrection`。ほかのキーが来たらその窓は閉じる）。取り消した読みは再訂正しない
  ・**直したら「打った読み → 直した読み」をカーソル下の小窓に出す**（`showTypoFeedback` /
    `TypoCorrectionFeedback`）。ライブ変換がONだと画面に出るのは変換後の文字列で、読みのどこが
    直ったかは示せない（未確定文字列の属性はアプリが無視することがあり当てにならない）。
    小窓の寿命は `typoUndoAvailable` に合わせる（**出ている ⇒ Backspace で戻せる**。逆は成り立たない。
    保険の4秒タイマーで閉じても取り消しは効かせたままにする）。小窓は予測変換と同じ1枚
    （`CaretPanel`）なので、訂正の小窓が出ている間は予測を出さない
  ・毎打鍵では走らせない。未解決のローマ字が残っている間（`composer.pending`）も走らせない
  ・**モデルが出力できない文字（数字・ASCII の記号・ヵヶ）は書き換えさせない**（`TypoNormalizer.correction`、2026-10-28）。
    こういう文字を含む読みは「入力のまま」の確率が 0 で margin が +∞ になり、θ と関係なく書き換わる
    （「11じにあいましょう」→「あじにあいましょう」）。数字の並びは「なん」に置き換えて読み全体を通し、
    「なん」の数が合えば数字に戻す（`NumberMask`。数字の位置で区切ると「にちに…」のような語の途中から始まる
    読みを渡すことになり、そこを書き換える）。それ以外の文字は、そこで区切って間の部分だけを通す
    （区切った部分にも最低文字数を当てる）。ローマ字は打ち間違いの印なので、これまでどおり直す対象。
    数字がローマ字の隣にある読み（「をk8たい」、i を 8 と打った）は数字も打ち間違いとみなし、読み全体をそのまま通す
  ・**読みが最低文字数（`TypoNormalizerSettings.minimumLength`、既定4・設定可）に満たなければ
    休止でもスペース押下でも走らせない**。短い読みは正しく打った語でも別の語の typo に見えやすく
    （「さど」→「さいど」→「再度」）、直されると打った語そのものが消える
  ・**入力中は「読みの末尾に足しただけ」の訂正を必ず捨てる**（`isTrailingInsertionOnly`）。
    学習データが句読点で終わる節なので、モデルは「こえて」→「こえて、」のように文を
    締めたがる。打ちかけの読みは常に終わりが足りなく見えるので、これは訂正ではなく補完
  ・休止地点の誤検出率は実測で低い（2026-09-21、`iroha-cli typo pause`、
    正しく打てている 302 件・θ=2.0）: 文節境界で切ると **1.29%**、打ち終わりで 0.66%。
    末尾追加を捨てないと 5.31% まで上がる。なお `iroha-cli typo prefix`（文字数で機械的に
    25/50/75% で切る）は 23.7/10.7/5.1% と厳しく出るが、**人が止まるのは語の途中ではなく
    文節の切れ目**なので、設計の判断には `typo pause` のほうを使うこと
  ・スペースを休止より先に押した場合の保険として、文節変換でも訂正を出す（こちらは読みを
    書き換えない）。`ConversionEngine` のデコレータ鎖には入れず、ユーザ定義ルールと同じく
    独立した候補生成源として候補ウィンドウに合流させる。
    読み全体の訂正を、差分が収まっている文節の候補に落とす（実測 93.2%）。
    差分が文節境界をまたぐ 6.8%（typo のせいで文節の切り方自体が崩れている
    「さsてえいただいていて」→「さ|sて|えいただいていて」のような場合）は、
    **文全体を訂正した候補**を先頭の文節に出し、選ばれたら文節ごと差し替える
    （`typoWholeSentence` / `applyTypoWholeSentenceIfSelected`。反映は
    `candidateSelectionChanged` が文節の結果を書き換える形なので、差し替えは `hidePanel` で行い、
    選んでいる間は後ろの古い文節を表示から隠す）。割合は `iroha-cli typo segments` で測れる。
    訂正候補を確定しても学習しない
    （`unlearnableCandidates`。覚えるとライブ変換に戻ってきて過剰訂正が表に出る）
  ・GPU に載せない（batch=1 の逐次デコードは CPU 1 スレッドが MPS の 5 倍速い）
  ・**モデルは .app に同梱しない。**設定でONにした時点でダウンロードする
    （`TypoNormalizerFetcher`（IrohaCore、取得・照合・設置）＋ `TypoNormalizerDownloader`（表示だけ））。
    重みは本体コード(MIT)と別ライセンス CC BY-SA 4.0 なので、配布物を分けてある。
    一覧は `models/typo-normalizer-format2.json`（v0.17 までのアプリは形式 1 の `models/typo-normalizer.json`。q6 の重みを
    読めないため、2026-11-11 に分けた。アプリが焼き込むのはこのURLだけ。モデルの追加・差し替えは
    カタログ更新だけで済む）、重み本体は Release タグ `typo-normalizer-v1`。
    公開は `./macos/scripts/publish-typo-normalizer.sh <書き出しdir> <ID>`。
    **必ず `--prerelease` で作ること**（通常リリースにすると `releases/latest` がこれを指し、
    `UpdateChecker` が壊れる）。設置先は `<データフォルダ>/models/typo-normalizer/` で、
    SHA-256 と大きさを照合してからでないと置かない。動作確認は `iroha-cli typo catalog [install]`
  ・**ライセンスをアプリに焼き込まない。**カタログのモデルごとに `license` / `attribution` を持つ。
    学習元を `dataset/iroha-typo-normalizer/` など別コーパスに替えたモデルは条件が変わりうるため。
    学習元は `sources`（1件ずつ名称・作者・ライセンス・URL）にも持ち、設定 > 情報 のライセンス一覧に出す
    （入れたときに設置記録にも写すのでオフラインでも出る）。文面は `dataset/iroha-typo-normalizer/LICENSES.md` の
    「attribution 方法」に合わせる。訂正モデルの行（入力タブ）には学習元を出さない
  ・訂正率 79%（θなし）は合成 typo 分布の数字で、実使用の数字ではない。UI で約束しない
- 記号・顔文字・絵文字の候補（`SymbolCandidateTable` / `SymbolCandidateMerger`、`macos/Sources/IrohaCore/SymbolCandidates/`、
  既定ON・切り替えは開発者モード・同期する。2026-10-16）は Mozc（BSD 3-Clause）の `src/data/` の 4 ファイルを
  `fetch-symbols.sh` がコミット固定で `vendor/mozc-data/` に取り、`make-bundle.sh` が `Contents/Resources/Symbols` へ
  LICENSE と一緒にコピーする（`dictionary_oss/` は第三者のデータなので使わない）。エンジンのデコレータ鎖には入れず、
  変換ルールと同じく文節の候補ウィンドウを開くときに合流させる: 絵文字は上から 7 番目（Mozc の `kDefaultInsertPos = 6`。
  今の表示・変換ルール・訂正がそれより下まであればその後ろ。azooKey 辞書が出した絵文字もここへ移す）、
  顔文字 → 記号はエンジンの候補の後ろ。「えもじ」「かおもじ」で全部。ライブ変換には使わず、表にある候補は
  確定しても学習しない（`unlearnableCandidates`。漢字・かなだけの候補は除く。覚えると「わらい」だけ打ったときに
  ライブ変換が絵文字になる）。macOS が描けない絵文字は OS の版の対応表ではなく Apple Color Emoji の字形で除く
  （`EmojiRenderability`。組の絵文字は字形 2 つで描かれるので「字形 1 つ」では判定しない）
- 専門辞書（`TermDictionary` / `TermDictionaryStore` / `TermDictionaryFetcher`、`macos/Sources/IrohaCore/TermDictionary/`、
  設定 > 辞書・学習 > 専門辞書、一般ユーザ向け。2026-11-08）は、配布している辞書を 1 つずつ追加・削除する。
  一覧は `models/term-dictionaries.json`、本体は Release `term-dictionaries-v1`（**必ず `--prerelease`**）、
  作り方は `macos/scripts/build-term-dictionaries.py`（fetch / build）と `publish-term-dictionaries.sh`。
  入れた辞書は `<データフォルダ>/dictionaries/<ID>.tsv` と `<ID>.json`（出典・ライセンスの記録）。守ること:
  ・**候補ウィンドウにだけ効かせる**（ユーザの判断）。辞書ラティスの候補に語として加わり、モデルの採点で並ぶ。
    ライブ変換の第一候補・ユーザ辞書の語の境目の判定（`wordBoundaries`）には入れない
  ・候補ウィンドウでは、専門辞書の語（か、その語＋読みの残りのかな）になっている候補の右に辞書の名前を小さな印で出す
    （`TermDictionaryLookup`。入れた辞書ファイルをマップして二分探索で引くので、辞書ファイルは読み・語の UTF-8 のバイト順に
    並べたままにする。`iroha-cli terms lookup <読み> <候補>…` で確かめられる）。基本の辞書にもある語（地名の「中央」など）にも付く
  ・辞書ラティスへは azooKey のユーザ辞書の形式（LOUDS、`DictionaryBuilder.exportDictionary`）に書き出して読ませる
    （`~/Library/Caches/iroha/term-dictionary-lattice/<組み合わせのハッシュ>/`、組み合わせごとに 1 回。全 38 万語で release 4.4 秒）。
    **動的ユーザ辞書（`importDynamicUserDictionary`）にしない**: ラティスは読みの頭からの部分が本体の辞書のどの語の頭とも
    一致しなくなった所で先を探すのをやめ、動的ユーザ辞書の語はその判定に加わらないので、多くの人名・地名に届かない
    （引くたびに全件をなめるので遅くもある）。LatticeConverter は本体の辞書を共有する変換器を 2 つ持つ
    （候補用は専門辞書を読む、境目の判定用は読まない）
  ・使う元データは再配布の条件を確かめたものだけ（JMdict CC BY-SA 4.0・百薬辞書 CC BY 4.0・郵便番号データ・Wikidata CC0）。
    万病辞書・JMED-DICT mini・ComeJisyo（条件を確認できない）、SKK の専門辞書（GPL v2）、Mozc の dictionary_oss は使わない。
    **JMdict のライセンスは配る版を新しく保つことを求める**ので、`.github/workflows/term-dictionaries.yml` が毎月作り直して
    公開しカタログをコミットする（アプリは設定画面を開いたときに入れ直す）。語の一覧（`contentSha256`）が前の版と同じ辞書は
    載せ直さない。新しい版のファイル名は `<ID>-<日付>-<ハッシュ8桁>.tsv`（同じ日に作り直しても公開中のファイルを上書きしない）。
    今と一つ前のカタログが指さないファイルは Release から消す。JMdict を取れなければ止め、ほかは前の版を使う
  ・ライセンスと出典（ライセンス文書の URL を含む）はカタログの辞書ごとに持ち、入れたときに記録へ写して 設定 > 情報 の
    ライセンス一覧に出す（アプリに焼き込まない）。辞書ファイルの先頭の「#」行と Release の NOTICE.txt に、条文が求める注記
    （著作権表示・EDRDG の出典の文・ライセンスへのリンク・加工した旨・無保証）を書く。注記の中身は
    `build-term-dictionaries.py` の `SOURCE_*` にまとめてある（2026-11-08 に条文を読み直して合わせた）
  ・iroha-cli は `IROHA_TERM_DICTIONARIES=installed`（入れた辞書）か `=a.tsv,b.tsv` で候補に加える（既定は使わない）。
    `iroha-cli terms list | install <ID> | remove <ID>` でアプリと同じ経路の取得を確かめる
- 自分の原稿に合わせる（`DocumentNotationSettings` / `DocumentNotationStore`、設定 > 辞書・学習の開発者モードの節、既定OFF・同期する。
  表記・用語の 2 つの重みは開発者モード・既定 2。2026-11-07）は、`<データフォルダ>/documents/` の文書（PDF・Word・テキスト。HTML は main thread
  でしか読めないので対象外）から読みごとの表記の回数（IrohaCore の `NotationPreference`）を数え、モデルの採点に
  「重み × Σ log((その表記の回数 + 1) / (表記の違いで最も多い表記の回数 + 1))」を足す（`NotationPreferenceEngine`。
  `LatticeRescoringEngine` の下、モデルの直上に置くので、第一候補は表記を替えた文と合わせて採点し直し、候補ウィンドウは
  ラティス候補の点数に足す）。文書の読み込み・分かち書き（`CFStringTokenizer`）は macOS 専用の `IrohaDocuments`。
  プライベートモードの間は止める。`documents/` の直下にフォルダを作ると、フォルダごとに分けて 1 つを選んで使う
  （`DocumentProfiles` / `DocumentProfileSelection`。論文と日常の切り替え用。2026-11-07）。フォルダがあると直下のファイルは
  使わない（共通の分として足すと算数が 1 段増えるため）。選択は Mac ごとで同期しない（キー `documentNotationProfile`、
  「使わない」は "/"、保存値が無い・消えたフォルダなら最初のフォルダ）。切り替えは設定画面・入力メニュー・ショートカット
  （既定 Ctrl+Option+D、使わない → 1 つ目 → … → 使わない）。「使わない」の変換は機能 OFF と同じ結果になるので、
  関係のない文の計測は要らない。必ず置き換えるユーザ辞書の形にしなかった理由と計測は
  `experiments/document-vocabulary/README.md`（リポジトリ外。論文の本文を含む）。守ること:
  ・**当てるのは同じ語の表記の違いだけ**（`NotationPreference.areVariants`: 漢字が包含関係、漢字とかなの組は漢字側に
    送り仮名があるとき）。同じ読みの別の語（両方／療法）に当てると、文書と関係のない文で別の語に替えた
    （iroha-bench-v1 で 27 項目）
  ・「に・て・で・と」の直後の語は数えず採点もしない（において・ていく・といった の かなが動詞をかなに寄せた）。
    読みが 1 拍の語（しゃ／者／車）と、その人が 1 通りしか書いていない語にも当てない
  ・変えたら **論文の文と文書と関係のない文の両方**で測る: `iroha-cli docs bench <evalset.jsonl> --weights …` と、
    `IROHA_DOCUMENTS=default IROHA_NOTATION_WEIGHT=2 iroha-cli ajimee testdata/{ajimee,iroha-bench-v1}/evaluation_items.json`。
    2026-11-07（論文 12 本・重み 2）: 論文の文 375 件 225 → 231、iroha-bench-v1 2,248 → 2,244、AJIMEE 172 → 170
  ・**用語（同じ読みの別の語。記譜／寄付）は話題で切り替える**（`topicWeight`、`DocumentTopicIndex`）。左文脈と、候補のどれにも
    共通する語から最も近い文書と近さ（入力の話題語のうちその文書に出る割合を idf で重み付け、0〜1）を求め、その文書での
    語の回数から見た不利さ（`NotationPreference.wordPenalty`）に「重み × 近さ」をかけて足す。判定している語そのものは
    話題の根拠にしない。比べるのは漢字・カタカナを含む語どうしだけ（ひらがなを含めると 又は→または・時→とき を替えた）。
    近さを文書全体との余弦類似度にすると 0.05〜0.2 にしかならず効かなかった。
    測り方は `docs bench --context previous --holdout sentence|document --topic-weights …`（sentence = 同じ原稿の続き、
    document = 別の話題の原稿）。2026-11-07（表記 2・用語 2）: 同じ原稿の続き 231 → 237、別の話題 229 → 230、
    iroha-bench-v1 は表記だけと比べて −1、AJIMEE +1。用語 4 は 241 だが研究報告の文で 期間→機関 などが出た
- ユーザ定義の変換ルール（User Rewriter、`UserRewriteRule` / `UserRewriteRuleStore`）は
  エンジンのデコレータ鎖に入れず、コントローラが文節の候補ウィンドウを開くときに独立した
  候補生成源として合流させる（第一候補の直後に挿入。ライブ変換には影響しない）。
  出力は `{{date:…}}` 等のテンプレートで毎回展開されるため、ルール由来の候補を確定しても
  学習には記録しない（`BunsetsuSegment.unlearnableCandidates` で識別）
- 区切り記号で囲んだ英字入力（`DelimitedAlphabetSettings`、開発者モードの項目・既定OFF・同期しない。2026-10-04）は、
  かな入力のまま開始の記号（既定「_」）から終わりの記号（既定「_」）までを打ったとおりの英字にする（「kyouha_iPhone_wo」）。
  記号はどちらも設定で変えられる（英字・数字以外の ASCII 記号 1 文字。使えない値なら既定）。中身は Shift+英字の
  `alphabetRun` と同じで、`alphabetRunDelimited` が真の間は Shift を離しても続く（表示は英字の前に開始の記号）。
  Shift+スペースで英字の中にスペース、開始の直後に終わりの記号（「__」）で記号そのものを読みに入れる。
  英字は `.alphabet` の固定部分（`delimited` つき）になるので、学習・入力履歴・打ち間違いの訂正の対象外は Shift+英字と同じ。
  英字を自動で判定する試み（打鍵どおりの英字の候補・休止での英単語の書き換え）は 2026-10-04 に取り下げた（d2cd6eb）
- プライベートモード（`PrivateModeSettings`、既定OFF・ON/OFF は同期しない・ショートカットは同期する。2026-10-16）は、
  ON の間、変換の学習（記録と差し替えの両方）・変換記録・入力履歴の記録と予測変換を止める。各機能の設定値は書き換えず、
  `LearningSettings.isActive` / `ConversionLogSettings.isActive` / `PredictionSettings.isActive` で止める
  （**記録・履歴に基づく提示を足すときは `isEnabled` ではなく、プライベートモードを見る `isActive` 側で判定する**）。
  2026-10-27 からユーザ辞書も止める（`PrivateModeSettings.userDictionary`。モードの間は変換を iroha 本来の結果にする
  というユーザの判断）。変換ルール（候補ウィンドウにだけ出る）・AI変換は止めない。切り替えは設定 > 辞書・学習・入力メニュー・ショートカット（既定 Ctrl+Option+P、
  IME に届いたキーで照合。グローバル登録はしない）。モード中の確定が入った `recentCommitted` は、OFF にした後の
  次の合成の開始で捨てる（変換記録の左文脈に残さないため）。
  ON の間はカーソルの右横に鍵のマークを出す（`PrivateModeIndicator`、設定 `privateModeCaretIndicator`・既定ON・同期する）。
  背景を付けず形だけにする（背景付きは右に続く文字を隠す）。横位置は `firstRect(forCharacterRange:)`、縦位置は
  `caretRect` から取る（テキストエディットで前者は縦が1行上、後者は横が1文字手前にずれた）。
  入力ソースのアイコン（あ / A）は Info.plist の入力モードに固定で実行中に差し替えられず、鍵付きの入力モードを足す方法は
  入力メニューから iroha が消える問題に触れるので採らない。メニューバーに別のアイコンを出す方法は分かりにくいので
  取り下げた（2026-10-16）
- 加速度センサ（`KeystrokeAccelerometer`・`AccelerometerSettings`、設定 > 情報 > 開発者向け「加速度センサを利用する」、
  開発者モード・既定OFF・同期しない・試験的。2026-10-24）は、MacBook（M2 以降）の非公開の加速度センサ（AppleSPUHIDDevice、
  用途ページ 0xFF00・用途 3）を読む。**管理者権限は要らない**（元にしたライブラリの説明と違う。MacBook で実測）。
  センサの値は専用スレッドで受ける（約 800 件/秒）。センサのない Mac では「（利用不可）」でトグルを操作できない。
  使い道は 2 つ: デバッグ表示（ON のとき）の下に直近 2 秒の揺れとキーの時刻を文字なしで描くグラフと、タップ入力。
  キーの名前 `developerOverlayAccelerometer` はグラフだけの設定だったときのもの。外すときの消す場所はファイル冒頭のコメント。
  計測ツールと実験の記録は `experiments/keystroke-force/`（README.md に経緯と数字）
- タップ入力（`TapInput` / `TapInputSettings`、判定は IrohaCore の `TapGestureDetector`、設定 > 入力 > タップ入力。
  加速度センサを利用するときだけ出す・既定OFF・同期しない。2026-10-24）は、⌘ を押したまま本体を指で 2 回叩いて ⌘ を離すと、
  設定したキーショートカット（`GlobalShortcut` の文字列形式）を合成したキーイベントで送る（アクセシビリティの許可が要る）。
  今はダブルタップの 1 種類だけ。数えたら（2 回以上のとき）カーソルの近くに「⌘ + ダブルタップ → Ctrl+Space」などを
  1.2 秒出す（`TapInputFeedback`。`CaretPanel` とは別の窓。カーソル位置は有効なコントローラが `attach` で渡し、
  取れなければマウスポインタの近く）。守ること:
  ・**⌘ を押している間だけ数える**。修飾キーなしでは手を置き直しただけの揺れもタップに見える。⌘ を押している間に
    他のキー・クリック・修飾キーが入ったら数えない（⌘C などを使った）。回数は ⌘ を離した時点で確定する（3 回目を待たない）
  ・キー・クリック・修飾キーの時刻は `CGEventSource.secondsSinceLastEventType`（権限不要）を 20ms ごとに読んで作る。
    ⌘ つきのキーは IME に届かないことがあるので、`handle(_:client:)` に届いたキーには頼らない
  ・`TapGestureDetector` の規則と既定値は実験の道具（`experiments/keystroke-force/` の `TapDetector`）と同じにする。
    一致は `TapGestureDetectorTests.matchesRecordedTrials`（実験の指示つきの記録で 75/80・73/80。記録がなければ何もしない）で確かめる。
    規則を変えたら、先に実験の道具の `analyze` で記録を数え直して比べる
- 「こえ」モード（`VoiceInput` / `VoiceInputSettings`、録音と認識は macOS 専用の `IrohaVoice`、認識結果を読みに分ける規則は
  IrohaCore の `VoiceTranscript`、設定 > 入力 > 「こえ」モード、一般ユーザ向け・既定OFF・同期しない・試作。2026-11-10、
  v0.17.0。2026-11-11 にユーザの判断で開発者モードから出した）は、決めた修飾キー（既定は右 ⌘）を押している間だけ録音し、離したら認識結果を未確定文字列に入れる
  （`IrohaInputController.insertVoiceTranscript`。確定はしない）。認識は macOS 26 の SpeechAnalyzer（端末内。
  `SpeechTranscriber` か `DictationTranscriber` を設定で選ぶ）。認識結果（漢字かな交じり）は `SystemReadingTokenizer` で
  読みに戻して合成中の読みの末尾に足し、ライブ変換に任せる。読みが付かない語（英字・数字）は `.alphabet` の固定部分にし、
  その前の読みは固定するときに変換しておく（`.liveConversion` の固定部分）。確認は
  `iroha-cli voice [--stream] <音声ファイル>… | --text <表記>`（読み上げ音声は `say -v Kyoko -o a.aiff "…"` で作れる。
  `--stream` はマイクと同じ経路で実時間の速さで渡し、途中結果が届いた時刻も出す）。守ること:
  ・**マイクの許可のダイアログを入力の途中で出さない**（未許可なら知らせるだけ。許可は設定画面から求める）。
    Developer ID で署名するときは `Resources/iroha.entitlements`（`com.apple.security.device.audio-input`）を付ける
    （hardened runtime ではこれが無いとダイアログも出ずに録音できない。`make-bundle.sh`）
  ・`recognizedEvents` に `flagsChanged` を足すのは設定が ON のときだけ（OFF のときのキーの流れを変えない）。
    押してから 0.12 秒待って録音を始め、その前に離した・押している間に他のキーやクリックがあった（ショートカット）・
    押していたのが 0.35 秒未満なら捨てる。キーが押されているかは `CGEventSource.flagsState` の左右を区別するビット
    （右 ⌘ 0x10・右 Option 0x40）で見て、取りこぼしに備えて 30ms ごとにも読む。**`CGEventSource.keyState` に右側の修飾キーの
    キー番号を渡しても偽しか返らない**（2026-11-10 実測。左側の番号に左右どちらの状態も出る。これで録音が一度も始まらなかった）。
    入力メソッドに届く `flagsChanged` の `modifierFlags` にも左右の区別は入っていない（キー番号は右 Option なら 61 で届く）
  ・音声認識のデータの状態（`AssetInventory.status`）はアプリごとで、Mac に入っていても取り置くまでは `.supported` と返る
    （2026-11-10 実測）。Mac に入っていれば「入っている」と扱い、使うときに取り置く（`VoiceRecognition.reserveAssets`）。
    ダウンロードは録音の途中では始めない
  ・読みの復元は取り違える（「何時」→ いつ、人名「立石」→ たていし → 停止）。数字の直後の漢字だけの語（年・月・日・人）は
    表記のまま数字とつなげる（切り離すと「10日」の日を か と読んだ）。読みの部分には認識の表記（`surface`）も持たせてあり、
    候補ウィンドウへの合流は未実装（次の段階）
  ・話している間は、音声認識の途中結果（表記）を未確定文字列の末尾に足して見せる（`voicePartial` / `showVoicePartial`。
    2026-11-10、長い発話では小窓の表示を見比べられないというユーザの指摘で、小窓から移した）。途中結果は表示だけで、
    合成の状態（読み・固定部分）には入れない。左文脈は最初の途中結果を出す前に読み、見せている間は読み直さない
    （未確定文字列として見せている途中結果が文脈に混ざる）。離して認識が終わると、読みに戻した合成に置き換わる
  ・入れたあと、音声認識の表記と iroha の変換の**違う箇所だけ**を小窓に出す（`VoiceComparison`。同じなら「音声認識と同じ」。
    試作の間の確認用）。合成が空のところへ入れたときだけ比べる。変換結果はライブ変換と同じ呼び出しをもう一度して求めている
  ・声で入れた読みを含む合成（`compositionHasVoice`）は、学習・変換記録・入力履歴に残さず、打ち間違いの訂正にも通さない
    （読みは求め直したもので、打ったものではない）
  ・`SpeechTranscriber` の途中結果は `.fastResults` を付けないと発話中に届かない（`.volatileResults` だけでは入力を締めた
    あとにまとめて届く。2026-11-10、`iroha-cli voice --stream` で再現）
  ・`SpeechTranscriber` は英字・数字の前に空白を入れ、句点・疑問符は文が終わったと見たときだけ付ける。
    `DictationTranscriber` は読点も付けるが、音声の終わりで最後の 1 拍を落とすことがあったので、終わりに無音 0.4 秒を足して締める
- 文節の打ち直し（`SegmentEdit` / `startSegmentEdit`。2026-11-10、v0.17.0）は、文節変換中（Space のあと）に
  文字を打つと、選んでいる文節をその文字から打ち直す（長い文の一部だけを直す。声で入れた文の聞き違いにも、
  打った文のライブ変換にも使う）。**それまでの「文字を打つと全文節を確定して次の入力を始める」は無くした**
  （ユーザの判断: 結局 Return で確定しているので要らない）。打つたびに**文全体（読みから変換した文節の全部）を
  新しい読みで変換し直し**、文節に分け直す（ユーザの指定。「こどもの公演に参加する」の「こどもの」を「だいがくの」に
  打ち直すと「大学の講演に参加する」。その文節だけを変換する第一版では後ろが「公演」のままだった）。
  Return で全体を確定、Space で今の文節の候補、←→ で隣の文節へ（いずれも打ち直しを終えてから処理する）。
  Esc で打ち直す前の文節列に戻す。空にして終えた文節は消える。
  文節変換中の Backspace も同じ仕組みで始める（`startSegmentEdit(nil, …)`。2026-11-11、声で入れた文の言いよどみを
  文節ごとに消したいというユーザの要望）: 選んでいる文節の読みを打ち直しの読みにして後ろから 1 文字消す。
  **それまでの「文節変換中の Backspace でかな入力に戻る」は、文節の区切りが届く前だけに残した**（かな入力に戻すのは Esc）。
  空になったあとの Backspace は何もしない（v0.17.0 では打ち直す前に戻していた。戻すと、押し続けたときに
  「消す → 戻る → 消す」を繰り返す）。最後の文節まで消して未確定文字列が空になったら文節変換を終える
  （`endSegmentModeIfEmptied`）。空にした文節を消したあとの ←→ は、選択がもう隣に移っているので 1 つ余分に動かさない。
  英字の文節を短くしただけなら英字の固定部分のままにして変換し直さない（`isAlphabetText`）。守ること:
  ・打ち直しの内容は 1 打鍵ごとに `segments` に写す（どの確定の経路も `segments` の表示のとおりに確定するため）
  ・固定部分（表示形を指定した部分・英字）は変換し直さず左文脈に使う。固定部分・AI の結果から作った文節を打ち直すときは、
    その 1 文節だけを変換する（`SegmentEdit.splits` が偽）
  ・ほかの文節で候補から選んであった表記も、変換し直すと変換結果で上書きされる（選択を保つ仕組みは無い）
  ・変換が届く前の表示: 読みが伸びただけで、カーソルのある文節の終わりが打ち直しの終わりと一致していれば、増えたかなを
    その文節の後ろに足す。一致していなければ（足す場所が読みの順とずれる）・文字を消したあとは、前後を打ち直す前の変換の
    ままにして打ち直しの読みをかなで見せる（`segmentEditSpan`）。Backspace では変換を止め、次の 1 文字で再開する
  ・終えたときに表示が今の読みの変換でなければ、範囲をもう一度変換し直す（`reconvertRetypedSpan`）。
    Space・↓ で終えたときはしない（そのあと開く候補の文節が途中で変わらないように）
  ・文節の区切りが届く前（`segmentationPending`。文節が読み全体の 1 つしかない）と、英字入力を始める文字
    （Shift+英字・区切り記号）は、今までどおり確定して新しい入力を始める
  ・終えたとき、全体の読み（`composer`）と固定部分を文節に合わせる（合わせないと、Esc でかなに戻したときに
    打ち直す前の読みが出る）
  ・打ち直した文節を含む確定（`segmentsRetyped`）は、学習・変換記録に残さない（読み全体が入力したときと変わっていて、
    学習の「読み全体 → 確定」「直す前の結果」と対応しない）。入力履歴には文節ごとに残す
- 確定の取り消し（Ctrl+Backspace、確定した直後だけ。`CommitUndo` / `undoCommit`。2026-10-27）は、`commitText` で
  確定する前の合成の状態を持っておき、カーソルの手前が確定した文字列のままなら（`attributedSubstring` で照合）
  それを消して状態を戻す。ほかのキー・クリック・フォーカス移動で取り消せなくする（`closeCommitUndo`）。
  **確定に伴う記録（学習・変換記録・入力履歴）は `deferCommitEffect` で包み、取り消せなくなったときに書く**
  （取り消した確定は本当の確定ではないので残さない）。確定の流れに記録を足すときもこれで包む
- AIで直す（`AIFixSettings` / `AIFixBadge`、判定は IrohaCore の `AIFixRule`、設定 > 入力、開発者モード・既定OFF・同期する。
  2026-11-07）は、入力の休止（打ち間違いの訂正と同じ待ち時間）でモデル単体の `ZenzEngine.convertWithConfidence` をもう一度呼び、
  その結果が表示中のライブ変換と一致し、文字ごとのマージンの最小値が閾値（既定 1.0）より小さければ、未確定文字列の右上に
  「AIで直す」を出す。クリックかショートカット（既定 Ctrl+Option+A。2026-11-08 から窓が出ていなくても入力中ならいつでも送る）で、前の文脈・読み（カタカナ）・
  変換結果を AI変換と同じ接続先に送り、案を `presentAIResultAsSegments` で文節にして差し替え、「今の変換 → AI の案」を
  `CaretPanel` に出す。直後の Backspace で元の変換に戻す。測定と設計の記録は `experiments/ai-fallback/`（リポジトリ外。
  README.md が測定、DESIGN.md が設計案）。守ること:
  ・**AI に送るのは押したときだけ**。窓を出すだけでは送らない。プライベートモードの間は出さない
  ・自動で書き換えない（測定では Opus 5.5 でも正しい変換を壊した。iroha-bench-v1 閾値 2.0 で直した 194・壊した 26）
  ・表示がモデルの出力と違う（学習・ユーザ辞書・原稿の表記で差し替わった）ときは疑わない。固定部分・英字・表示形の上書き・
    80文字を超える読みも対象外（第一版の制限）。判定のマージンは推論エンジンに合わせて llama.cpp か MLX（`MLXConversionEngine.convertWithConfidence`、2026-11-08 追加）で求める
  ・依頼の文面と答えの読み方（複数行なら最後の行）は測定と同じにしてある。変えたら測り直す
  ・AI の案で確定した文は学習にも変換記録にも残さない（`segmentsFromAI`）
- 候補ウィンドウの辞書ラティスには azooKey の特別な候補（`LatticeConverter.specialCandidateProviders`、2026-10-27）を
  入れている: 桁区切り（「12345」→ 12,345）・時刻（「1230」→ 12:30）・和暦西暦（「2026ねん」→ 令和8年）。
  和暦西暦だけは azooKey の実装ではなく iroha の `EraYearCandidates`（2026-11-13）: azooKey v0.11.2 は明治の年で
  1967 を引いていて「1900ねん」が「明治-67年」になるので、同じ候補を作る部品を書き写して直した。
  Windows 版（`windows/iroha-core/src/lattice/special_candidates.cpp`）と同じ規則にしてある。変えるときは両方を合わせる
- 変換・予測の左文脈は合成開始時にアプリのカーソル手前テキストを1回読む（`DocumentContextSettings.read`、
  既定ON）。読めないアプリでは `recentCommitted`（確定文字列の蓄積）に代える。合成中は読み直さない。
  左文脈は**カーソルのある行だけ**（`LeftContext.normalize`、azooKey と同じ。2026-09-30 に「改行を除いて前の段落と
  つなげる」から変更。前の行の「神と化しています」に引かれて「かみとかしています」を誤変換したため）。
  `recentCommitted` も合成していないときの Enter で空にする
- 学習は「文節変換の結果がエンジンの出力と違ったら記録」。1件は「入力の読み全体 → 確定文字列」と
  「直す前にエンジン（学習を除く）が出していた結果」（`LearningEntry.replaced`）で、次に同じ読みを丸ごと入力し、
  エンジンがまた同じ結果を出したときだけ差し替える（2026-09-30。「ほうこう→芳香」を覚えても、左文脈「御恩と」で
  エンジンが「奉公」を出せば差し替えない。差し替えるかをエンジンの結果で決めるので、学習済みの読みでもエンジンは毎回呼ぶ）。
  記録時の「直す前」は `LearningEngine.engineResult(forReading:shown:)` で引く。差し替えた結果をエンジンの結果に
  戻して確定したらその学習を消す（`LearningStore.forget`）。差し替えた表示を F6-F10 / Ctrl+U…T で別の表示形にして
  確定したときも消す（`RejectedLearning` / `forgetRejectedLearnings`。2026-10-27。それまでは Space の候補から
  戻したときだけで、「は→派」をひらがなにして確定しても残った）。`replaced` が nil の学習（それより前のもの）は
  読みが一致すれば常に差し替え、同じ読みで「直す前」つきの学習を記録したときに置き換える。文節単位の学習（＋直前の確定文字列を文脈に
  持つ仕組み）は 2026-09-18 に廃止した（設定画面で同じ語が「全体」「文節」の2行に見え、
  適用条件の文脈がユーザから見えないため）。旧形式の `learning.json` は読めるが
  `kind == "segment"` のエントリは読み込み時に捨てる
- 学習用データの記録（`ConversionLog`、既定OFF・設定は同期しない）は**左文脈のある確定だけ**を残す
  （`ConversionLogSettings.shouldRecord`。文脈なしの例は追加学習に使えない）。
  記録と学習（`learning.json`）はどちらも設定画面から一覧・編集・削除できる
  （`ConversionLogView` / `LearningView`。`ConversionLog.records()` / `replace` / `delete`、
  `LearningStore.replaceAll`）。`ConversionLog.Record` は書き戻しの照合に `original` を使うので、
  呼び出し側は `entry` を書き換えてそのまま渡してよい。`learning.json` とは別の追記専用JSONL
  （`<データフォルダ>/logs/conversions/`）。修正なしの確定も含めて、モデルに渡した左文脈・読み・提示・確定を残す。
  `learning.json` は辞書なので上書き・上限・マージがあり、学習データ用途にはそのまま使えない
- 変換記録からの追加学習（LoRA）は `macos/Sources/IrohaTrain/`（MLX Swift、mlx-swift **0.31.4 固定**）と
  実行ファイル `iroha-train`（`Contents/MacOS/`。学習そのものは別プロセス）。学習した LoRA は llama.cpp の
  アダプタ形式 GGUF（`LoraAdapterWriter`）に書き、`ZenzEngine(adapterPath:)` が適用する（設定キー `modelAdapterPath`、
  `modelPath` と同じく再起動で反映）。MLX の Metal カーネルは `swift build` では作れないので
  `./macos/scripts/build-mlx-metallib.sh` が `.build/<config>/mlx-swift_Cmlx.bundle/default.metallib` を生成する
  （install.sh / release.yml が呼ぶ。**Metal Toolchain が必要**: `xcodebuild -downloadComponent MetalToolchain`）。
  テストは `swift test --filter IrohaTrainTests`（metallib が debug 側に無ければスキップ。
  `./macos/scripts/build-mlx-metallib.sh debug` で作る）。MLX 実装と llama.cpp のロジット一致（`GPT2ParityTests` / `T5ParityTests`）を
  崩さないこと。対応アーキは gpt2（zenz）と t5（`T5Model`、training/t5/ の自作モデル。2026-09-26 追加）。
  T5 はエンコーダ入力 = U+EE01 まで + `</s>`、デコーダ = 開始トークン + 出力 + `</s>` で学習する
  （`TrainingDataBuilder.encodeEncoderDecoder`。`ZenzEngine` と `train_t5.py` と同じ形）。LoRA の対象層は
  アーキごとの既定（`TrainableLM.defaultLoRATargets`、ブロック内の線形層すべて）。
  `T5ParityTests` は training/t5/ の GGUF（リポジトリ外）が無ければスキップする。
  別アーキ（llama など）を足すときは `TrainableLM` の実装を追加して `TrainableModels.load` に登録する
- かな漢字変換の推論は llama.cpp（`ZenzEngine`、既定）と MLX（`MLXConversionEngine`、`macos/Sources/IrohaMLX/`）から
  選べる（設定 > モデル > 推論エンジン、キー `inferenceBackend`。機械ごとの設定で同期しない・再起動で反映。2026-10-01 追加）。
  **MLX は T5 だけ**。デコーダ 2 層の T5 では 1 ステップの手間が llama.cpp より小さく、生成が約 2 割速い
  （T5 e12d2 最終で 30.6 → 24.0ms、AJIMEE 2 条件とも出力が全問一致。`training/t5/MAC-AJIMEE-MLX-2026-10-01.md`・
  `MAC-SPEED-MLX-vs-LLAMA-2026-10-01.md`）。12 層の zenz では逆に遅かったので IME では使わない（MLX を選んでいても
  T5 以外は `InferenceBackend.resolve` が llama.cpp に戻し、設定画面に理由を出す。`MLXConversionEngine.supportedArchitectures`）。
  ただし計測のため zenz（GPT-2）も読める（`loadableArchitectures`、iroha-cli の `IROHA_BACKEND=mlx`。2026-10-31。
  f32 で llama.cpp と 312 項目の出力が全部一致、bf16 では 7 項目が違う）。IrohaCore は MLX に依存させない
  （Windows 移植の候補のため。MLX 版は IrohaMLX → IrohaTrain の `T5Model` / `GPT2Model` を使う）。守ること:
  ・プロンプト・読み制約・終端・n-best・一括採点の規則は `ZenzEngine` と同じにする。一致は `MLXConversionEngineTests`
    （f32 で ZenzEngine と突き合わせ）と `T5ParityTests` で確かめる。片方だけ変えない
  ・計算は bf16。**T5 は f16 だと途中の値が溢れて全問空の出力になる**（llama.cpp は途中を f32 で計算するので起きない）
  ・配列の形は刻みにそろえる（KV は 64 単位で確保して位置を差し替える、エンコーダ入力は 16 の倍数、採点は候補数 2 の累乗・
    長さ 16 の倍数、出力層は使う位置だけ）。MLX は解放したメモリをほぼ同じ大きさにしか使い回さない。
    KV の書き込みに MLXArray の添字代入を使わない（オブジェクトをその場で書き換えるので、分岐した状態が壊れる）
  ・使い回し用のメモリは `Memory.cacheLimit` で縛らず、変換の切れ目で 128MB を超えていたら手放す（`trimCache`）。
    上限で縛ると生成の小さな配列が使い回されず llama.cpp より遅くなる。何もしないと 1GB 近くまで膨らむ
  ・重みは f16 のまま 1 テンソルずつ読んで bf16 にし、読み込みは `autoreleasepool` で包む（FileHandle の Data が
    自動解放のまま全テンソルぶん残り、常駐メモリが 200MB 余り増えた）。llama.cpp は重みをファイルからマップするので
    プロセスの使用量は MLX より小さい（AJIMEE 中のピーク: llama.cpp 80MB、MLX 447MB。MLX は重みをメモリに持つ）
  ・LoRA アダプタは読み込み時に重みへ足し込む（`LoRAAdapterMerger`。当たらないテンソルがあれば例外）。
    量子化済み GGUF は `ModelRequantizer.ensureF16` の f16 版を読む
  ・CLI は `IROHA_BACKEND=mlx`（`IROHA_MLX_DTYPE=f32` で f32）
- 追加学習の流れは 4 段（`TrainingRun`）: ① 記録をベースモデル（アダプタなし）で1件ずつ変換し直す
  （`TrainingScreener`）→ ② 間違いの一部（1/3・最大25件）と正解の一部（最大40件）を評価用に取り分ける
  （`TrainingDataBuilder.stratify`）→ ③ **残りの記録すべて**を訓練データにして学習 → ④ 評価用をアダプタなし／ありで
  変換して並べる（`TrainingEvaluator`）。間違いの重み付け・アンカー比率は **2026-09-18 に廃止**した
  （少ない記録で効果を出す細工が UI を説明不能にしていた。記録は増えるので、増えた分だけ効く設計にする）。
  損失は確定文字列だけにかかり条件はその人の左文脈なので、正解できていた記録も「その人の文脈 → 確定」を学ばせる
  1 サンプルである（間違いだけで学習すると偏った尾部に過剰適合し、忘却として現れる）。
  変換し直しが要るのは、`ConversionLogEntry.edited` が辞書ラティス・学習・ユーザ辞書を含むエンジン全体の
  提示に対する差分で NN の誤りと一致しないため（実測: 直した 11 件のうち NN は 6 件を既に正解、逆に NN が
  間違える 12 件のうち 7 件はユーザが直していない）。**`edited` を学習対象や評価の判定に使わないこと**。
  結果は 2 群（間違えていた変換・できていた変換）でアダプタなし／ありの一致数を並べて報告する。
  エポック数・学習率は設定画面（`TrainingSettings`、同期しない）→ `iroha-train --epochs/--lr`。
  UI の文言は「記録を貯めるほど効く」を前に出す（少ない記録で効かせる方向に設計を戻さない）。
  zenz-v3.1-small は個人の記録の 96.9% を既に正解するので、学べる差分は少ない（2026-09-18 実測）
- 既定のかな漢字変換モデルは **iroha-kkc-v1**（Q5_K_M。2026-11-06 に iroha-t5-alpha から変更。その前は 2026-10-03 まで
  zenz-v3.1-small。文字単位 T5 e12d2、`training/t5/`）。初回起動時に `ModelDownloader` が取得し、大きさと SHA-256 を照合してから
  `<データフォルダ>/models/iroha-kkc-v1-Q5_K_M.gguf`（`DataDirectory.defaultModelFileName`）に置く。重みは CC BY-SA 4.0
  （学習元 zenz-v2.5-dataset を継承）なので本体と分け、Release の専用タグ `iroha-kkc-v1` に置く。
  前の既定 iroha-t5-alpha の Release `kkc-model-v1` は、配布済みのアプリが固定の URL で取得するので消さない・名前を変えない。
  **このタグは必ず `--prerelease` で作る**（`releases/latest` を奪うと `UpdateChecker` が壊れる。typo-normalizer-v1 と同じ）。
  既定のモデルを差し替えるときは、ファイル名・URL・大きさ・SHA-256 を `ModelDownloader.swift` と
  `macos/scripts/fetch-model.sh` の両方で変え、設定 > 情報 のライセンス一覧（`SettingsView` の `LicenseRow`）と README も合わせる
- バージョンはgitタグが唯一の情報源。リリースはCIがタグから、開発ビルドは
  install.shがgit describeから注入する（Info.plistのコミット値はフォールバック。
  リリース時にゆるく追随させる）。Windows版も同じ原則でCIがタグから注入すること。
  リポジトリの引用情報 `CITATION.cff` の `version` と `date-released` も、リリースのたびに合わせる
  （GitHub の「Cite this repository」に出る。論文が公開されたら `preferred-citation` に論文を足す）

## Windows版の開発（windows/、Windowsマシン上のClaude向け）

- WindowsのIMEはTSF（Text Services Framework）ベースで `windows/` に実装する。
  macOSのIMK部分（`macos/Sources/iroha/`）は流用不可
- 候補ウィンドウの辞書ラティスはmacOS版ではAzooKeyKanaKanjiConverter（Swift）を使っている。
  Windows版はその変換の部分をC++に移したもの（`windows/iroha-core/src/lattice/`、入口は `LatticeConverter`。
  2026-11-13、ユーザの判断: 辞書ラティスはほぼ直さないので書き写す）で、同じ辞書データを読む
  （`windows/scripts/fetch-dictionary.ps1`、コミットは `macos/scripts/fetch-dictionary.sh` と合わせる）。
  **移植が正しいかの判定は `iroha-cli lattice-parity <macOS版の lattice-dump の出力>`**（候補の並びまで一致するか。
  AJIMEE 200件で一致を確認済み）。macOS側でAzooKeyKanaKanjiConverterの版や `LatticeConverter.swift` の
  呼び出し方（オプション・特別な候補）を変えたら、Windows側も合わせて `lattice-parity` を回す。
  専門辞書をラティスに読ませる部分は未移植（`windows/README.md`）
- Windows版の `ZenzEngine`（`windows/iroha-core/src/zenz_engine.cpp`）は `ZenzEngine.swift` を移したもので、
  T5（既定の iroha-kkc-v1）と候補の採点に対応している（2026-11-13）。macOS側でプロンプト・読み制約・終端・n-best・
  採点の規則を変えたら Windows側も合わせ、`iroha-cli model-parity`（macOS版の ajimee-dump の出力との照合）と
  `iroha-cli model-selfcheck`（生成と採点の辻褄）を回す。**採点は macOS版と違い候補を1つずつ行う**
  （llama.cppのCPU版は1バッチに載せるトークン数で値が動くため。理由と実測は `windows/README.md` の「変換モデル」節）。
  変換サーバのエンジンの重ね方・未移植のもの（学習とユーザ辞書の新仕様・長い読みの分割・異体字など）も同じ節にある
- 変換エンジンは選択肢が2つ: `macos/Sources/IrohaCore/` をSwift for Windowsで共有
  （Foundation + llama.cppのみ依存で移植可能な設計）、または別言語で再実装。
  **方針は未決定。実装前にユーザーに確認する**
- llama.cppのソースはルートの `vendor/llama.cpp` を共有（zenz対応パッチ
  `patches/llama-cpp-zenz-pretokenizer.patch` の適用が必要）。ただし
  `vendor/dist` はmacOS用（Metal依存）なので**Windows用は別ディレクトリ
  （例 `vendor/dist-windows`）にビルドし、`vendor/dist` を上書きしない**
- 変換の仕様（zenzのプロンプト形式・読み制約・学習/ユーザ辞書のデコレータ構成）は
  README.mdの「開発」節と `macos/Sources/IrohaCore/` の実装が正。挙動を合わせること
- GGUFモデル・評価データ（`testdata/`）・学習パイプライン（`training/`）は
  プラットフォーム非依存でそのまま使う。`training/` は学習作業中のことがあるので
  **明示的な指示なしに変更しない**
- リリースは上記「両プラットフォームのリリース方針」に従う（単一タグ・単一Release）

## 自作変換モデル（文字単位T5）の学習（training/t5/、2026-09-11設計）

「T5の学習を始めて」「パイロットを回して」等の指示があったときの手順。詳細・設計根拠・
判断の目安は `training/t5/README.md` が正。

- 学習は **GPUマシン上**で行う（このMacはMPSで疎通確認のみ）。学習用venvは GPU側の
  `/home/baba/venvs/imellm-training`、リポジトリは `/data1/Dropbox/project/iroha`（Dropbox同期）。
  Mac側のClaudeから直接は起動できないので、実行コマンドをユーザーに渡す（または GPU側の
  Claude に指示する）形になる
- 前提: llm-jp の full ラン（`training/iroha-llmjp-150m-full/`）が終わっていること
  （同居させるかは要確認。`ls training/iroha-llmjp-150m-full/` の checkpoint 更新が止まっていれば終了）
- 順序: ① `train-10m.txt` でパイロット（本番形状 enc12/dec2×768、1エポック）→
  ② AJIMEE で zenz-xsmall（68.5%）を超えたら `train-full.txt` で本番 → ③ 途中チェックポイントも
  `training/convert-gguf.sh <ckptディレクトリ>`（`tokenizer.model` と `config.json` を添える）で
  GGUF化し `macos/scripts/bench-compare.sh` で zenz-small と比較
- **学習したモデルと zenz の比較は必ず2条件で分けて報告する**: 「NNのみ」（`IROHA_LATTICE=off`）と
  「辞書ラティス + NN再採点」（`IROHA_LATTICE=always`）。`macos/scripts/bench-compare.sh` が両条件を
  1表に出す。基準は zenz-v3.1-small / xsmall（Q5_K_M）。`iroha-cli bench / ajimee` はユーザ辞書・学習を
  既定で空にする（`IROHA_WITH_USER_DATA=1` で本体データ）。手順と基準値は `training/README.md` の
  「比較の標準条件」が正
- 新しい語彙（トークナイザ）のモデルを評価するときは、`IROHA_NO_CONSTRAINT=1`（読み制約なし）と既定を
  比べて制約の損失がないことを先に確認する（2026-09-16: llm-jp の語頭「▁」スペースを制約が弾いて
  約10pt失っていた。`training/README.md` 参照）
- 評価で見るもの: 学習損失・eval loss・AJIMEE acc@1 の三つを並べる（llm-jp では損失が下がっても
  AJIMEE が横ばいだった。200件のノイズ幅は±5pt）。**モデルどうしの差は iroha-bench-v1 で判断する**
  （`testdata/iroha-bench-v1/`、学習に入っていない 3,253 項目。`experiments/iroha-bench/run.sh` で測り、
  基準との差を McNemar 検定で出す。2026-10-29 に作成。AJIMEE では差が揺れに埋もれる。作り方は
  iroha-dataset の `build-iroha-bench`、結果と注意点は `experiments/iroha-bench/README.md`）。レイテンシは bench の平均。30ms を超えるなら
  データや幅より先にデコーダ 1 層を試す
- 学習データ（`train-*.txt`）・`prepare_data.py`・`train.py` は llm-jp 側と共有。**変更しない**。
  T5 側の変更は `training/t5/` の中だけで行う
