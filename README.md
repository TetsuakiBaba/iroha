# iroha（いろは）

ローカルLLMをかな漢字変換エンジンとして用いるmacOS用日本語IME。

- **ライブ変換**: ことえりのライブ変換のように、入力に追従して読みをLLMへ送り、変換結果をリアルタイム表示。Enterで確定
- **候補変換**: スペースキーでn-best候補を候補ウィンドウに表示
- **文脈対応**: 直前に確定した文字列を条件としてLLMに与え、文脈に合った変換を行う
- **ユーザ辞書**: 固有名詞などを登録して変換に反映。macOSのユーザ辞書（システム設定 > キーボード > ユーザ辞書）から取り込める
- **学習**: 文節変換で修正して確定した変換を覚え、次から第一候補にする。同じ読みでも文中の位置で使い分ける（「きしゃのきしゃ」→「記者の貴社」）
- **変換ルール**: よみ（トリガー）と出力の組を登録し、一致したとき出力を候補に加える。`{{date:yyyy/MM/dd}}` `{{time:HH:mm}}` のように変換のたびに変わる動的な出力が書ける（「きょう」→ 2026/09/06）
- **AI変換して確定**: 修飾キー+Returnで、未確定文字列をAIに渡して確定。プロンプトを3つまで登録でき、1つ目は英訳が入っている（Apple Intelligence / Ollama / LM Studio）
- **ローカル動作**: 変換モデルは [zenz-v3.1-small](https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf)（GPT-2系95M・GGUF・約70MB）を llama.cpp（Metal）で実行。実測レイテンシは1変換あたり15〜30ms程度（Apple Silicon）

## インストール

- macOS 14以降 / Apple Silicon（リリースビルドは arm64 のみ）

1. [Releases](https://github.com/TetsuakiBaba/iroha/releases/latest) から `iroha-x.y.z.zip` をダウンロードして解凍
2. `iroha.app` をダブルクリック — 自動で `~/Library/Input Methods/` にインストール・登録される
3. **システム設定 > キーボード > 入力ソース > 編集 > + > 日本語 > iroha** を追加
   （一覧に出ない場合は一度ログアウト/ログインする）

変換モデル [zenz-v3.1-small](https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf)（約72MB、CC-BY-SA-4.0）は初回起動時に自動ダウンロードされる。
アップデートは新バージョン公開時に自動で通知される（メニューバーの入力ソースアイコン >
「アップデートを確認...」で手動確認も可能）。

### アンインストール

**設定 > 情報 > 「irohaをアンインストール...」** から実行できる（入力ソースからの削除と
アプリ本体の削除まで自動。ユーザ辞書などのデータも一緒に消すかは選択できる）。
手動で行う場合は、システム設定の入力ソースからirohaを外したうえで
`~/Library/Input Methods/iroha.app` と `~/Library/Application Support/iroha` を削除する。
アクセシビリティ権限の項目（選択テキストのAI編集を使った場合）は
「システム設定 > プライバシーとセキュリティ > アクセシビリティ」から手動で削除する。

## ソースからのビルド

- macOS 14以降（Apple Silicon推奨）
- Xcode（Swiftツールチェーン）
- CMake（`brew install cmake`、llama.cppのビルドに使用）

```sh
# 1. llama.cppをスタティックビルド（初回のみ、zenz対応パッチを適用）
./macos/scripts/build-llama.sh

# 2. 変換モデル(zenz-v3.1-small)をダウンロード（初回のみ、約70MB）
./macos/scripts/fetch-model.sh

# 3. ビルドして ~/Library/Input Methods/ にインストール
./macos/scripts/install.sh
```

その後、**システム設定 > キーボード > 入力ソース > 編集 > + > 日本語 > iroha** を追加する
（一覧に出ない場合は一度ログアウト/ログインする）。

## 使い方

| キー | 動作 |
|---|---|
| ローマ字入力 | かなに変換され、続けてLLMがライブ変換 |
| Enter | 表示中の変換結果を確定 |
| Space | 文節変換モードへ（文節に分割して表示、もう一度Spaceで候補ウィンドウ） |
| ← / → | 文節間を移動（文節変換中、選択中の文節は太い下線） |
| Shift+← / Shift+→ | 選択中の文節の区切りを伸縮（以降の文節は自動で再変換） |
| ↑↓ / Space | 候補ウィンドウ内の選択（Enterで採用、Escで閉じる） |
| Esc | 候補→文節→かな→取消 と段階的に戻る |
| Backspace | 1文字削除。表示はかなに戻り、次の入力までライブ変換しない（文節変換中はかな入力に戻る） |
| 、。！？ | 自動確定（メニューでOFF可、ライブ変換時のみ） |
| F6 / F7 / F8 | ひらがな / カタカナ / 半角カタカナ（Ctrl+U / I / O でも可） |
| F9 / F10 | 全角英数 / 半角英数（打鍵通りの文字列、Ctrl+P / T でも可） |
| Shift+英字 | Shiftを押している間だけ英字入力。文章の途中でも確定せずに英字を挿入できる（下記） |
| 修飾キー+Return（設定で変更可） | 未確定文字列をAI変換して確定（既定は ⌃Return で英訳。処理中はEscで取消） |
| 英数 / かな | 英数モード / ひらがなモード切替（JISキーボード） |
| Ctrl+Shift+J / ; | ひらがな / 英数モード切替 |

F6-F10で指定した表示は、続けて入力してもその形のまま保持される（ライブ変換の対象から外れ、
以降の変換の文脈としてだけ使われる）。Backspaceでその部分まで戻るか、Escを押すとかなに戻る。

Shift+英字（大文字）を打つと英字入力になり、それまでのかなはその時点のライブ変換結果で固定される。
Shiftを押している間は数字・記号も英字のまま末尾に足され、Shiftを離して打った文字からはかな入力に戻る
（英字部分は固定され、以降の変換の文脈としてだけ使われる）。
`kyouha` `Shift+A` `Shift+I` `wotsukau` Enter → 「今日はAIを使う」のように、
英数モードへの切り替えや途中確定なしに1回のEnterで確定できる。
英字入力中にSpaceを押すとその英字の文節が選ばれ、もう一度Spaceで 打鍵通り / 小文字 / 大文字 / 先頭だけ大文字 / 全角 の候補が出る。
Backspaceは英字を1文字ずつ消し（固定した英字部分に戻ってきたときも同様）、英字が無くなるとShiftを押す前の状態に戻る。
英字入力中のEscはその英字だけを取り消す。固定済みの英字部分はEscでかなには戻らない。

文節分割は「変換結果中のひらがな（助詞・送り仮名）を読みと突き合わせる」軽量な
アライメント（[ReadingAligner](macos/Sources/IrohaCore/ReadingAligner.swift)）による初期推定で、
Shift+←→でユーザがいつでも調整できる。文節の候補生成は選択中の文節の読みと
左側の確定済み文字列を文脈としてLLMに与えて行う。

メニューバーの入力ソースアイコンのメニューの「設定...」から設定ウィンドウを開ける。
項目は5つのタブに分かれている:

| タブ | 内容 |
|---|---|
| 入力 | ライブ変換・句読点で自動確定・候補数・句読点スタイル（、。/ ，．） |
| 辞書 | ユーザ辞書（編集・macOSからの取り込み）・変換ルール（編集）・学習（ON/OFF・リセット） |
| AI | AI変換して確定のプリセット3つ（名前・プロンプト・ショートカット）・AIサービスの選択 |
| モデル | かな漢字変換モデルのパス・ダウンロード状況・再起動 |
| 情報 | アップデート確認・バージョン・データの保存場所（iCloud/Dropboxで共有）・クレジット |

パスワード欄（Secure Input）ではmacOSがIMEをシステムレベルで無効化するため、
iroha側の対応は不要。

### AI変換して確定

修飾キー+Returnで、未確定文字列をAIに渡し、返ってきた結果を確定する。
英訳も敬語化も要約も「違うプロンプトを投げているだけ」なので、
**名前・プロンプト・ショートカット**の組を3つまで登録できる形にしてある
（1つ目は既定で英訳のプロンプトが入っている。中身は自由に書き換えてよい）。

```
あした会議やるから来て
  1. 英訳   ⌃Return    -> Come to the meeting tomorrow.
  2. 敬語   （設定）    -> 明日の会議に参加してください。
  3. 要約   ⇧⌃Return   -> 明日の会議の出席依頼。
```

- プロンプトに `{text}` と書くとその位置に未確定文字列が入る（無ければプロンプトに続けて渡される）
- ショートカットは ⌃ / ⌥ / ⇧ / ⌘ / ⇧⌃ / ⇧⌥ / ⇧⌘ + Return から選ぶ。
  同じキーは2つに割り当てられない（片方を選ぶともう片方がオフになる）
- バックエンドは3つ共通で、設定の「AIサービス」で選ぶ:
  Apple Intelligence（オンデバイス、macOS 26+）/ Ollama / LM Studio。
  Ollama / LM Studio では利用可能なモデルを自動取得して一覧から選ぶ
  （thinking対応モデルでも思考過程は出力されない）
- 処理中は結果がストリーミング表示され、Escで取り消せる。
  失敗・タイムアウト時は元の日本語がそのまま確定される

### 学習

文節変換（スペースキー）で候補を選び直したり文節を伸縮したりして確定すると、
その修正を学習して次から第一候補にする。同じ読みでも文中の位置で使い分けるため、
文節ごとに「直前までに確定した文字列」を条件として一緒に覚える。

```
きしゃのきしゃ -> 貴社の貴社        （学習前）
   ↓ スペースキーで「記者の」に修正して確定
きしゃのきしゃ     -> 記者の貴社     （入力全体が一致するので即座に、LLMを呼ばない）
きしゃのきしゃがきた -> 記者の貴社が来た （文節の学習が連鎖して当たる）
あのきしゃにのる   -> あの汽車に乗る  （文脈が違うので学習は当たらない）
```

エンジンの出力をそのまま確定した場合は何も覚えない（修正したときだけ学習する）。
設定でOFFにでき、「リセット」で全消去できる。

### ユーザ辞書

メニューの「ユーザ辞書...」（または設定ウィンドウの「編集...」）から、よみと単語を登録できる。
登録した単語は、読み全体が一致すればLLMを介さずそのまま変換結果になり、
文の一部が一致する場合は一致部分を単語で埋めて残りだけをLLMが変換する
（例:「きららざかにいく」→「雲母坂」+「に行く」）。

「macOSのユーザ辞書から取り込む」で、システム設定 > キーボード > ユーザ辞書 に
登録済みの単語を取り込める（設定でONにすれば起動時に自動同期）。読み取り専用で、
macOS側の辞書は変更しない。ローマ字入力では到達できないよみ（"omw" のような
ASCIIショートカット）は取り込みの対象外。取り込んだ単語をirohaで編集すると
以後の同期では上書きされない。

### 変換ルール（User Rewriter）

メニューの「変換ルール...」（または設定 > 辞書 > 変換ルール > 「編集...」）から、
**トリガー（よみ）と出力**の組を登録できる。文節の読み全体がトリガーに完全一致すると、
出力を展開した文字列が候補ウィンドウの2番目（第一候補の直後）に入る。
ユーザ辞書と違って出力は変換のたびに計算されるので、日付や時刻のように
動的に変わる文字列を出せる。ルールごとに有効/無効を切り替えられる。

```
きょう -> {{date:yyyy/MM/dd}}          2026/09/06
あした -> {{date+1}}                   2026/09/07
いま   -> {{time:HH:mm}}               14:05
ひづけ -> 本日（{{date:M月d日(E)}}）   本日（9月6日(日)）   ← 通常の文字と混在できる
われき -> {{wareki:Gy年M月d日}}        令和8年9月6日
```

- プレースホルダは `{{名前:書式}}`。`date`（既定 `yyyy/MM/dd`）・`time`（`HH:mm`）・
  `datetime`（`yyyy/MM/dd HH:mm`）・`wareki`（和暦、`Gy年M月d日`）。書式はICUの日付パターンで、省略可
- `date` / `datetime` / `wareki` は名前に `+N` / `-N` を付けてN日ずらせる
  （`{{date+1}}` 明日、`{{date-2:M月d日}}` 一昨日、`{{wareki+3}}` 明々後日）
- 初回起動時に既定のルールが入る: きょう・きのう・おととい・あした（あす）・あさって・しあさって（日付）、
  いま（時刻）。不要なものは編集画面で消せる（消したものは復活しない）
- 対応していないプレースホルダは書いたまま出る（編集画面のプレビューで赤く表示）
- ライブ変換の表示や第一候補は変えない（かな漢字変換・LLMの候補はそのまま）。
  ルールの出力を選んで確定しても学習には記録しない（日付を覚えると翌日から誤るため）
- トリガーは今のところ完全一致のみ（正規表現・コード実行・外部通信はしない）

### データの保存場所（複数のMacで共有）

ユーザ辞書・学習・変換ルール・変換モデル・設定は、既定では
`~/Library/Application Support/iroha` に保存される。
**設定 > 情報 > データの保存場所 > 「フォルダを変更...」** でiCloud DriveやDropboxの中の
フォルダ（例: `~/Dropbox/iroha`）を指定すると、複数のMacで同じデータを使える。

- 変更時に今のデータをコピーするか選べる。移行先に既にファイルがあれば上書きしない
  （先に別のMacが置いたデータが優先される）。変更後はirohaが自動で再起動する
- 他のMacからの同期で辞書・学習・ルールのファイルが変わると、再起動なしで読み直す。
  学習は両方のMacの内容をマージする（同じ読み・文脈は新しい方を採る）
- 設定（ライブ変換・候補数・AIプリセット・選択テキストのAI処理など）はフォルダ内の
  `settings.json` を介して同期される。保存場所そのもの・モデルの絶対パス・アップデート確認の履歴・
  APIキー（キーチェーン）は端末ごとの値なので同期しない
- 2台で同時に編集した場合は後に保存した方が残る（Dropboxでは「競合コピー」が残ることがある）
- 保存場所を変えていると、アンインストール時の「データも含めて削除」は既定の場所だけを消し、
  共有フォルダには触らない

## 開発

macOS版のSwiftパッケージは `macos/` 配下にある（Windows版は今後 `windows/` に実装予定）。
ビルドコマンドは `macos/` から実行する:

```sh
cd macos
swift test                                 # 単体テスト（ローマ字変換など）
swift build && .build/debug/iroha-cli repl  # CLIで変換を試す（レイテンシ表示付き）
.build/debug/iroha-cli convert --n 5 "きしゃ"          # n-best候補
.build/debug/iroha-cli convert --context "天気予報によると" "きしょう"  # 文脈条件付け
log stream --predicate 'process == "iroha"' --style compact  # IMEのログ
```

- 変換エンジンは [ConversionEngine](macos/Sources/IrohaCore/ConversionEngine.swift) プロトコルで抽象化されており、
  [ZenzEngine](macos/Sources/IrohaCore/ZenzEngine.swift)（zenz-v3 + llama.cpp）を別モデルに差し替えられる
- モデルの評価は `iroha-cli bench ../testdata/eval.tsv`（完全一致率・CER・レイテンシ）。
  自作モデルの学習パイプライン（データ準備→学習→GGUF変換→評価）は [training/](training/README.md) を参照
- データフォルダは既定で `~/Library/Application Support/iroha`（設定で変更可。
  [DataDirectory](macos/Sources/IrohaCore/DataDirectory.swift) が唯一の参照点で、以下のパスはすべてここから導く。
  iroha-cliはIME本体の設定に従い、環境変数 `IROHA_DATA_DIR` で上書き可）
- モデルファイルは `<データフォルダ>/models/` に置く（環境変数 `IROHA_MODEL` で上書き可）
- 学習結果は `<データフォルダ>/learning.json`（環境変数 `IROHA_LEARNING` で差し替え可）
- 変換ルールは `<データフォルダ>/user-rewrite-rules.json`
  （[UserRewriteRule](macos/Sources/IrohaCore/UserRewriteRule.swift)。変換エンジンのデコレータ鎖には入れず、
  コントローラが候補ウィンドウを開くときに独立した候補生成源として合流させる。
  トリガーの一致方法は `TriggerKind`、テンプレートへ渡す値は `RewriteMatch.parameters` で拡張する）
- ユーザ辞書は `<データフォルダ>/user-dictionary.json`（環境変数 `IROHA_USER_DICT` で
  iroha-cli から差し替え可）。macOSのユーザ辞書の実体は `~/Library/KeyboardServices/TextReplacements.db`
  （非公開スキーマのSQLite。実データが未チェックポイントのWALにあるため db/-wal/-shm ごとコピーして読む）
- zenzのプロンプト形式: `[U+EE02 + 左文脈] + U+EE00 + カタカナ読み + U+EE01 → 変換結果`
- 生成は読みで縛る（[ReadingConstraint](macos/Sources/IrohaCore/ReadingConstraint.swift)）。
  ひらがな・句読点は読みと一致する位置でしか出せず、読みを使い切るまで終端させない。
  これがないと「こんにちはあかちゃん → こんにちは。赤ちゃん」のように読みにない文字が混ざる
- 候補ウィンドウの候補は辞書ラティス（[LatticeConverter](macos/Sources/IrohaCore/LatticeConverter.swift)、
  [AzooKeyKanaKanjiConverter](https://github.com/azooKey/AzooKeyKanaKanjiConverter) + azooKey辞書）で作り、
  zenzの対数確率で並べる（[LatticeRescoringEngine](macos/Sources/IrohaCore/LatticeRescoringEngine.swift)）。
  読み制約は漢字・英字の読みを検証できないため、zenzのn-bestだけだと「ないようを → 活用を / NIPPON」の
  ような読みの合わない候補が混ざる。辞書ラティスの候補は読みが保証されるので、それをモデルで順位付けする
  （Zenzaiと同じ役割分担）。zenz自身の生成結果も一緒に採点するので、辞書にない語も候補に残る。
  ライブ変換（第一候補）はzenzの生成のまま: AJIMEE-Bench 200件で zenz生成 84.5%、
  ラティス候補の再採点は 66.5%（長い文では10件のn-bestに正解が入らない）。
  辞書は `macos/scripts/fetch-dictionary.sh` で `vendor/azooKey_dictionary_storage` に取得し
  （コミット固定、約35MB）、`make-bundle.sh` が `iroha.app/Contents/Resources/Dictionary` へコピーする。
  生の候補と採点は `iroha-cli lattice [--context 文脈] <読み>` で見られる
- zenzのn-best（辞書が無いときの候補ウィンドウ）は先頭トークンを上位から分岐して貪欲に補完し、
  系列の対数確率で並べ、最良候補から8nat以上離れた候補は捨てる
- 長い読みは区切って順に変換する（[ChunkedConversionEngine](macos/Sources/IrohaCore/ChunkedConversionEngine.swift)）。
  zenzはおおむね80文字を超える読みで途中や末尾を飛ばし始めるため、モデルに渡す読みを50文字以下に保つ。
  区切りは句読点の直後、なければ窓を変換して文節境界（ReadingAligner）で切り、前の区切りの結果を左文脈にする。
  先頭側の区切りはキャッシュするので、ライブ変換で打鍵ごとに再変換されるのは末尾の区切りだけ
- llama.cppにはzenzのpre-tokenizer名（`gpt2-small-japanese-char`）を認識させる
  [パッチ](patches/llama-cpp-zenz-pretokenizer.patch)を当てている（build-llama.shが自動適用）

## リリース手順（メンテナ向け）

バージョンは **gitタグが唯一の情報源**。タグをpushすると
[release.yml](.github/workflows/release.yml) がビルド→Developer ID署名→公証→GitHub Release作成まで自動実行する:

```sh
git tag v0.4.0 && git push origin v0.4.0
```

- `CFBundleShortVersionString` はタグから、`CFBundleVersion` はワークフローの実行番号から注入される
  （`macos/Resources/Info.plist` のコミット値は開発ビルドの表示用）
- `-` を含むタグ（例 `v0.4.0-beta.1`）はプレリリースになり、アップデート通知の対象外
  （アップデータのQAは `defaults write dev.iroha.inputmethod.iroha updateCheckURL <リリースAPIのURL>` で行う）
- 署名関連のSecrets: `MACOS_CERTIFICATE_P12` / `MACOS_CERTIFICATE_PASSWORD` / `MACOS_SIGN_IDENTITY` /
  `APPLE_TEAM_ID` / `NOTARY_KEY_ID` / `NOTARY_ISSUER_ID` / `NOTARY_KEY_P8`。
  証明書を更新した場合、Team IDが変わると `UpdateChecker.swift` の `expectedTeamID` も更新が必要
  （不一致だとセルフアップデートの署名検証が通らなくなる）

## ライセンスと帰属

- 本リポジトリのコード: [MIT](LICENSE)
- 変換モデル [zenz-v3.1](https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf)（Keita Miwa氏）: CC-BY-SA-4.0
- [llama.cpp](https://github.com/ggml-org/llama.cpp): MIT
- 辞書ラティス [AzooKeyKanaKanjiConverter](https://github.com/azooKey/AzooKeyKanaKanjiConverter)（ensan / azooKey）: MIT
  （依存の [swift-algorithms](https://github.com/apple/swift-algorithms)・[swift-collections](https://github.com/apple/swift-collections)・
  [swift-tokenizers](https://github.com/ensan-hcl/swift-tokenizers): Apache-2.0）
- 辞書データ [azooKey_dictionary_storage](https://github.com/azooKey/azooKey_dictionary_storage): Apache-2.0
- アプリアイコン・メニューバーアイコンの書体 [Tsukimi Rounded](https://fonts.google.com/specimen/Tsukimi+Rounded)（Takashi Funayama氏）: SIL Open Font License 1.1
- 設計にあたり [azooKey-Desktop](https://github.com/azooKey/azooKey-Desktop) / Zenzai の公開知見を参考にした
