# iroha 開発ガイド（Claude Code用）

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
cd macos && swift build && swift test   # ビルドと単体テスト（必ず macos/ から実行。
                                        # Package.swiftのvendor/dist参照がcwd相対のため）
./macos/scripts/install.sh       # ローカルの ~/Library/Input Methods/ へインストール（ad-hoc署名）
/usr/bin/log stream --predicate 'process == "iroha"' --style compact  # IMEログ

# 設定ウィンドウだけを開く（IMEに接続しない。UI確認用）
"$HOME/Library/Input Methods/iroha.app/Contents/MacOS/iroha" --settings [input|dictionary|selection|model|about]
```

- `log` はzshの組み込みと衝突するため必ず `/usr/bin/log` をフルパスで呼ぶ
- NSLogはこの環境ではユニファイドログに残らない。IMEの実行時調査は
  /tmpへのファイル直書きヘルパーを一時的に仕込む（調査後に削除）
- 起動・終了・異常終了の履歴は `<データフォルダ>/logs/launch-<ホスト名>.log`（`LaunchLog`）。
  予期しない再起動やクラッシュの調査はまずここと `~/Library/Logs/DiagnosticReports/iroha-*.ips` を見る

## プロジェクト構成の要点

- プラットフォーム別レイアウト: macOS版のSwiftパッケージ一式（Package.swift / Sources /
  Tests / Resources / scripts）は `macos/` 配下。Windows版は今後 `windows/` に実装する。
  `vendor/`（llama.cpp）・`patches/`・`testdata/`・`training/`・`.venv` はプラットフォーム共有の
  ためリポジトリ直下に置く（**`training/` のスクリプトがルート直下の `vendor/`・`.venv` を
  参照しているので、これらを `macos/` 配下へ移動してはならない**）
- **`training/` と `experiments/` はリポジトリに入っていない**（2026-09-22 に追跡から外した。
  他者のデータを iroha のリポジトリから再配布しないため。`.gitignore` の該当行に理由がある）。
  中身が要るときは `git checkout v0.12.0 -- training experiments` で履歴から取り出す。
  - このMacの `training/` は**第一階層のファイルだけ**が Dropbox で同期されている
    （1.0GB。`train.py` `prepare_data.py` `README.md` と GGUF・ログ類）。
    `t5/` や `iroha-llmjp-150m-full/` などの**サブディレクトリは同期していない**ので手元に無い。
    T5 側の作業や学習そのものは GPU マシン（`/data1/Dropbox/project/iroha`）で行う
  - `experiments/` はこのMacに全部ある（2.4GB）。追跡していないだけ
- `macos/Sources/iroha/` — IME本体（Swift 5モード）: IMKコントローラ、設定UI、
  AIバックエンド（Apple FoundationModels / Ollama / LM Studio / OpenAI互換）、
  アップデータ、モデルDL、macOSユーザ辞書の取り込み（`SystemUserDictionary`）、
  選択テキストのAI編集（`Selection/`。グローバルショートカットとマウス選択で
  他アプリの選択テキストをAIで置換する。GenGoの機能を移植。既定OFF・要アクセシビリティ権限）
- `macos/Sources/IrohaCore/` — 変換エンジン（Swift 6モード）: zenz + llama.cpp。
  Foundationのみ依存でmacOS専用APIは不使用（将来のWindows移植候補）
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
  学習 → ユーザ辞書 → 長い読みの区切り（`ChunkedConversionEngine`、50文字超を
  句読点/文節境界で分割して逐次変換）→ LLM の順に重ねる。ユーザ辞書は読みを分割して
  一致部分を埋め、学習は読み全体が一致したときだけ差し替える）。
  macOS側の辞書は読み取り専用で絶対に書き込まない。
  ユーザ辞書のうち単語が読みより大幅に長いエントリ（`UserDictionary.isSuitableForLiveConversion`
  が偽: 2倍超かつ3文字以上長い。ハッシュタグ・URL・定型文など）はライブ変換・文節分割には
  使わず候補ウィンドウにだけ出す（ことえりと同じ体感）。この語を候補から確定しても学習しない
  （学習は辞書の外側にあるので、覚えるとライブ変換に戻ってくる）
- 打ち間違いの訂正（`TypoNormalizer`、`macos/Sources/IrohaCore/TypoNormalizer/`、既定OFF）は
  かな漢字変換の**手前**で「読み → 読み」を直す 3.2M の文字単位 Transformer（実装は Accelerate の
  `cblas_sgemm` だけ。llama.cpp も MLX も通さない）。学習は `experiments/typo-normalizer/`、
  **移植が正しいかの判定は `iroha-cli typo parity`（PyTorch 実装との照合 200 件）で行う。**
  `experiments/typo-normalizer/SWIFT-PORT.md` は移植を頼むときに書いた開発機間の伝言メモで、
  リポジトリには入っていないし、実装が進んだ今は内容が古い。ソース中の
  `experiments/typo-normalizer/…` への参照も出自を示すもので、clone には含まれない
  （読みたいときは作業機か `git checkout v0.12.0 -- experiments`）。守ること:
  ・**計算の順序を変えたら `iroha-cli typo parity` を必ず回す**（PyTorch 実装との照合 200 件。
    生成が一致してもロジットがずれていれば実装は間違っている。float32 でロジット 1e-3・logP 0.01 以内）
  ・**本線は「入力の休止」で読みそのものを直す**（`scheduleTypoCorrection`、既定 300ms・設定可）。
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
    一覧は `models/typo-normalizer.json`（アプリが焼き込むのはこのURLだけ。モデルの追加・差し替えは
    カタログ更新だけで済む）、重み本体は Release タグ `typo-normalizer-v1`。
    公開は `./macos/scripts/publish-typo-normalizer.sh <書き出しdir> <ID>`。
    **必ず `--prerelease` で作ること**（通常リリースにすると `releases/latest` がこれを指し、
    `UpdateChecker` が壊れる）。設置先は `<データフォルダ>/models/typo-normalizer/` で、
    SHA-256 と大きさを照合してからでないと置かない。動作確認は `iroha-cli typo catalog [install]`
  ・**ライセンスをアプリに焼き込まない。**カタログのモデルごとに `license` / `attribution` を持つ。
    学習元を `iroha-dataset/` など別コーパスに替えたモデルは条件が変わりうるため
  ・訂正率 79%（θなし）は合成 typo 分布の数字で、実使用の数字ではない。UI で約束しない
- ユーザ定義の変換ルール（User Rewriter、`UserRewriteRule` / `UserRewriteRuleStore`）は
  エンジンのデコレータ鎖に入れず、コントローラが文節の候補ウィンドウを開くときに独立した
  候補生成源として合流させる（第一候補の直後に挿入。ライブ変換には影響しない）。
  出力は `{{date:…}}` 等のテンプレートで毎回展開されるため、ルール由来の候補を確定しても
  学習には記録しない（`BunsetsuSegment.unlearnableCandidates` で識別）
- 変換・予測の左文脈は合成開始時にアプリのカーソル手前テキストを1回読む（`DocumentContextSettings.read`、
  既定ON）。読めないアプリでは `recentCommitted`（確定文字列の蓄積）に代える。合成中は読み直さない
- 学習は「文節変換の結果がエンジンの出力と違ったら記録」。1件は「入力の読み全体 → 確定文字列」で、
  次に同じ読みを丸ごと入力したときだけ再現する。文節単位の学習（＋直前の確定文字列を文脈に
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
  実行ファイル `iroha-train`（`Contents/MacOS/`、IME 本体は MLX をリンクしない）。学習した LoRA は llama.cpp の
  アダプタ形式 GGUF（`LoraAdapterWriter`）に書き、`ZenzEngine(adapterPath:)` が適用する（設定キー `modelAdapterPath`、
  `modelPath` と同じく再起動で反映）。MLX の Metal カーネルは `swift build` では作れないので
  `./macos/scripts/build-mlx-metallib.sh` が `.build/<config>/mlx-swift_Cmlx.bundle/default.metallib` を生成する
  （install.sh / release.yml が呼ぶ。**Metal Toolchain が必要**: `xcodebuild -downloadComponent MetalToolchain`）。
  テストは `swift test --filter IrohaTrainTests`（metallib が debug 側に無ければスキップ。
  `./macos/scripts/build-mlx-metallib.sh debug` で作る）。MLX 実装と llama.cpp のロジット一致（`GPT2ParityTests`）を
  崩さないこと。別アーキ（llama / T5）を足すときは `TrainableLM` の実装を追加して `TrainableModels.load` に登録する
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
- バージョンはgitタグが唯一の情報源。リリースはCIがタグから、開発ビルドは
  install.shがgit describeから注入する（Info.plistのコミット値はフォールバック。
  リリース時にゆるく追随させる）。Windows版も同じ原則でCIがタグから注入すること

## Windows版の開発（windows/、Windowsマシン上のClaude向け）

- WindowsのIMEはTSF（Text Services Framework）ベースで `windows/` に実装する。
  macOSのIMK部分（`macos/Sources/iroha/`）は流用不可
- 候補ウィンドウの辞書ラティスはmacOS版ではAzooKeyKanaKanjiConverter（Swift）を使っている。
  C++再実装では同等の辞書ラティス（azooKey辞書のLOUDS形式を読むか、別の辞書）が別途必要
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
  AJIMEE が横ばいだった。200件のノイズ幅は±5pt）。レイテンシは bench の平均。30ms を超えるなら
  データや幅より先にデコーダ 1 層を試す
- 学習データ（`train-*.txt`）・`prepare_data.py`・`train.py` は llm-jp 側と共有。**変更しない**。
  T5 側の変更は `training/t5/` の中だけで行う
