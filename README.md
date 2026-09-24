# iroha（いろは）

ローカルLLMをかな漢字変換エンジンとして用いるmacOS用日本語IME。

- **ライブ変換**: ことえりのライブ変換のように、入力に追従して読みをLLMへ送り、変換結果をリアルタイム表示。Enterで確定
- **候補変換**: スペースキーでn-best候補を候補ウィンドウに表示
- **文脈対応**: カーソル手前にあるアプリの文章（読めないアプリでは直前に確定した文字列）を条件としてLLMに与え、文脈に合った変換を行う
- **ユーザ辞書**: 固有名詞などを登録して変換に反映。macOSのユーザ辞書（システム設定 > キーボード > ユーザ辞書）から取り込める
- **学習**: 文節変換で修正して確定した変換を、読み全体と対にして覚え、次に同じ読みを入力したとき第一候補にする
- **変換ルール**: よみ（トリガー）と出力の組を登録し、一致したとき出力を候補に加える。`{{date:yyyy/MM/dd}}` `{{time:HH:mm}}` のように変換のたびに変わる動的な出力が書ける（「きょう」→ 2026/09/06）
- **選択した文字数の表示**: どのアプリでも、マウスで選択したテキストの文字数を選択範囲の近くに表示（設定でON・要アクセシビリティ権限）
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
- Xcode（Swiftツールチェーン）と Metal Toolchain（Xcode 26 では別コンポーネント。
  `xcodebuild -downloadComponent MetalToolchain`。追加学習ヘルパーのMetalカーネルのコンパイルに使用）
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
| F6 / F7 / F8 | ひらがな / カタカナ / 半角カタカナ（Ctrl+U / I / O でも可） |
| F9 / F10 | 全角英数 / 半角英数（打鍵通りの文字列、Ctrl+P / T でも可） |
| Shift+英字 | Shiftを押している間だけ英字入力。文章の途中でも確定せずに英字を挿入できる（下記） |
| 修飾キー+Return（設定で変更可） | 未確定文字列をAI変換して確定（既定は ⌃Return で英訳。処理中はEscで取消） |
| Tab | カーソル下の小窓に出た予測（予測変換・インライン補完、設定でON）を取り入れる。小窓と候補ウィンドウはクリック・スクロール・アプリ切替でも閉じる |
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

### 予測変換とインライン補完（設定でON、既定OFF）

- **予測変換（入力中）**: 入力を止めると（既定300ms、設定で100〜2000ms）、ライブ変換の結果に続く
  次の文節をカーソル行の直下の小さなウィンドウに表示する（例:「今日は」→「いい」）。
  Tabで取り入れると未確定文字列の一部になり、そのまま入力を続けられる（続けてTabを押すと文が伸びる）。
  取り入れた部分は読みを持たないので、BackspaceまたはEscで予測ごと取り消す（Tabを押す前の表示に戻る）。
  取り入れた部分を含む確定は学習しない。ライブ変換がONのときだけ動く
- **インライン補完（確定後）**: 確定して操作を止めると、確定した文章の続き（次の文節）を同じ
  ウィンドウに表示する。Tabで確定（挿入）、それ以外のキーではまず閉じてからそのキーが普通に処理される
  （Escは閉じるだけ）。Tabを押すまでアプリのテキストには一切触らないので、検索欄のインクリメンタル
  検索などが予測文に反応することはない
- どちらも「、」「。」などの句読点が出たらそこまでを予測する。予測は1文節（短ければ2文節）
- ウィンドウの位置はアプリが返すカーソルの矩形で決める。カーソル位置を返さないアプリでは出ない
- モデルはかな漢字変換とは別に指定できる（設定 > モデル。既定は同じzenzを共有）

文節分割は「変換結果中のひらがな（助詞・送り仮名）を読みと突き合わせる」軽量な
アライメント（[ReadingAligner](macos/Sources/IrohaCore/ReadingAligner.swift)）による初期推定で、
Shift+←→でユーザがいつでも調整できる。文節の候補生成は選択中の文節の読みと
左側の確定済み文字列を文脈としてLLMに与えて行う。

メニューバーの入力ソースアイコンのメニューの「設定...」から設定ウィンドウを開ける。
項目は5つのタブに分かれている:

| タブ | 内容 |
|---|---|
| 入力 | ライブ変換・候補数・打ち間違いの訂正（ON/OFF・休止時間・確信の強さ）・予測変換・インライン補完・予測の休止時間・句読点スタイル（、。/ ，．）・AI変換して確定のプリセット3つ（名前・プロンプト・ショートカット） |
| 辞書・学習 | ユーザ辞書（編集・macOSからの取り込み）・変換ルール（編集）・変換の学習（ON/OFF・リセット）・変換記録（ON/OFF・削除） |
| 選択テキスト | 選択テキストのAI編集（有効化・権限・トリガー・プリセット5つ・除外するアプリ）・選択した文字数の表示 |
| モデル | かな漢字変換モデルのパス・ダウンロード状況・再起動・予測変換/インライン補完のモデル・AIサービスの選択 |
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

### 変換の学習

文節変換（スペースキー）で候補を選び直したり文節を伸縮したりして確定すると、
その修正を学習して次から第一候補にする。覚えるのは「入力した読みの全体 → 確定した文字列」の
1件だけで、次に同じ読みを丸ごと入力したときに再現する。

```
きしゃ -> 汽車            （学習前）
   ↓ スペースキーで「貴社」に修正して確定
きしゃ       -> 貴社      （読み全体が一致するので即座に、LLMを呼ばない）
あのきしゃ   -> あの汽車  （読み全体が違うので学習は当たらない。LLMが変換する）
```

読みの一部には当てはめないので、学習が入力の途中に割り込んで変換を壊すことがない。
エンジンの出力をそのまま確定した場合は何も覚えない（修正したときだけ学習する）。
設定でOFFにでき、一覧（**設定 > 辞書・学習 > 変換の学習**）で1件ずつ直す・消すほか、
「リセット」で全消去できる。

### 変換記録（設定でON、既定OFF）

上の学習とは別に、確定した変換を1件ずつファイルに記録できる
（**設定 > 辞書・学習 > 変換記録**）。修正の有無にかかわらず、ライブ変換の確定・文節変換の確定・
F6〜F10による確定を、そのときモデルに渡した左文脈（カーソル手前の文章の末尾40文字）・読み・
モデルが提示していた変換結果・確定した文字列とともに残す。変換の動作には影響しない。
記録はこのMacの中に残るだけでどこにも送信せず、あとで自分の入力に合わせた変換モデルの
追加学習（LoRAなど）の素材に使うためのもの。

- **左文脈のある確定だけを記録する**（起動直後やフォーカス移動直後の1語目は記録しない）。
  追加学習は「この文脈でこの読みならこう変換する」を学ぶので、文脈のない例は学習データにならない
- 保存先はデータフォルダ内の `logs/conversions/conversions-<ホスト名>-YYYY-MM.jsonl`（1行1件のJSON、月ごと）。
  書いていた文章の一部がそのまま入るので既定OFFで、この設定は他のMacに同期しない
- **「確認・編集...」で一覧を見て、読みや確定を直したり、1件ずつ削除できる**
  （打ち間違いをそのまま確定した記録が学習に混ざるのを防ぐ）。「削除...」で全消去
- 「記録する範囲」で「直した確定だけ」を選ぶとファイルは小さくなるが、追加学習では
  「モデルが間違えるのに自分は直さなかった変換」（辞書や学習に助けられていたもの）を学べなくなり、
  「壊れていないか」の確認にも使える記録が減る。既定の「すべての確定」を薦める
- 上限や間引きはしない。1件は300〜400バイト程度で、毎日たくさん書いても年に数百MB以内
- 変換ルールの出力（日付など）や候補ウィンドウ専用のユーザ辞書語、AI変換・英訳での確定は記録しない

### 自分の入力で追加学習（LoRA、Apple Silicon）

変換記録が溜まったら、**設定 > モデル > 自分の入力で追加学習** の「学習を開始」で、使用中の変換モデルに
自分の入力を反映した LoRA アダプタを作れる。ベースのモデルは変えず、小さなアダプタファイル（数MB）を
`<データフォルダ>/models/adapters/` に書き、「このアダプタを使う」→再起動で変換に適用される。

- 学習はこの Mac の GPU（MLX）で行い、どこにも送信しない。記録 1100 件・3 エポックで 3 分半ほど（M 系 Mac、実測 2026-09-18）
- 手順は 4 つ:
  1. 記録を1件ずつ今のモデル（アダプタなし）で変換し直し、**いまのモデルが間違えるもの**を見つける
  2. 間違いの一部（新しい方から 3 分の 1・最大 25 件）と、正解できていた記録の一部（最大 40 件）を
     評価用に取り分ける
  3. **残りの記録すべて**で学習する（エポック数と学習率は「学習の設定」で変えられる。既定 3 / 1e-4）
  4. 評価用の記録をアダプタなし／ありで変換し、一致した数を並べて見せる
- 訓練データは記録そのままで、間違いを重み付けしたり正解を間引いたりしない。学習は「自分の左文脈とよみ」を
  条件に「自分が確定した文字列」を当てる形なので、正解できていた記録も自分の文脈・言葉づかいを学ばせる
  材料になる。**記録が増えるほど効く**設計で、少ない記録で効果を出そうとする細工はしない
- 変換し直しが必要なのは、記録の「直したか」がエンジン全体（辞書ラティス・学習・ユーザ辞書を含む）の提示に
  対する差分で、モデル自身の誤りとは一致しないため（実測では「自分が直した 11 件」と「モデルが間違える 12 件」は
  半分しか重ならなかった）
- 結果は 2 行で出る。「間違えていた変換」が何件当たるようになったか（効果）と、「できていた変換」が
  何件保てたか（副作用）。体感では悪化の方が目立つので必ず両方見る
- アダプタを使っている間に記録された確定も分け隔てなく学習に使う
  （記録は「ユーザがこれでよいと思って確定した結果」なので、どのモデルが候補を出したかは関係ない）
- 使った記録は `<アダプタ>.train.tsv` / `.mistakes.tsv` / `.correct.tsv` に残る。
  何を覚えさせたかを確かめられる（**自分の打ち間違いをそのまま確定した記録も学習対象になる**ので、
  変なものが入っていたらアダプタを使わないか、記録を削除して学習し直す）。
  `iroha-cli bench <tsv>` で同じ数値を再現できる
- 対応するのは zenz（gpt2 アーキテクチャ）のモデル。記録 30 件以上で学習できる
- 学習は別プロセス `iroha-train`（バンドル内）で動くので、途中でキャンセルしても入力には影響しない
- 効き方の目安: zenz-v3.1-small は個人の記録の 97% を既に正解できる（実測 373/385）ので、
  学べる差分は少ない。専門用語が多い分野や、より小さいモデルを使うときに余地が大きい

### ユーザ辞書

メニューの「ユーザ辞書...」（または設定ウィンドウの「編集...」）から、よみと単語を登録できる。
登録した単語は、読み全体が一致すればLLMを介さずそのまま変換結果になり、
文の一部が一致する場合は一致部分を単語で埋めて残りだけをLLMが変換する
（例:「きららざかにいく」→「雲母坂」+「に行く」）。
ただし単語が読みより大幅に長いエントリ（読みの2倍超かつ3文字以上長いもの。
「たぐ」→「#helloworld #dummytag」のようなハッシュタグ、メールアドレス、定型文など）は
ライブ変換には使わず、候補ウィンドウにだけ出る（ことえりと同じ挙動）。

「macOSのユーザ辞書から取り込む」で、システム設定 > キーボード > ユーザ辞書 に
登録済みの単語を取り込める（設定でONにすれば起動時に自動同期）。読み取り専用で、
macOS側の辞書は変更しない。ローマ字入力では到達できないよみ（"omw" のような
ASCIIショートカット）は取り込みの対象外。取り込んだ単語をirohaで編集すると
以後の同期では上書きされない。

### 変換ルール（User Rewriter）

メニューの「変換ルール...」（または設定 > 辞書・学習 > 変換ルール > 「編集...」）から、
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

### 打ち間違いの訂正（設定でON、既定OFF）

**設定 > 入力 > 打ち間違いの訂正** をONにすると、**入力の手が止まったとき（既定300ms）に
読みの打ち間違いを直してから変換する**。かな漢字変換の**手前**で「読み → 読み」を直す専用の
小さなモデル（3.2Mパラメータの文字単位Transformer）を使う。

```
をわぇてけいやくする  →  候補ウィンドウに「分けて」（訂正後の読み「をわけて」の変換結果）
がちがうんっだろな    →  「んだろな」（「っ」の入れすぎ）
fとんはあらったほうが →  「布団は」（ローマ字がかなにならず残った打ち間違い）
```

- 直せるのは隣のキーの打ち間違い・抜け・重複・入れ替え・「っ」の過不足。ローマ字が
  かなにならずに残った文字（`とうじょうsじない`）もひらがなに直せる
- **直したときは「打った読み → 直した読み」をカーソルの下の小窓に出す。** ライブ変換がONだと
  画面に出るのは変換後の文字列なので、読みのどこが直ったかは読みの形でしか見せられない
  （未確定文字列の色や下線はアプリが無視することがあり、当てにできない）。
  小窓はBackspaceで戻せる間だけ出る（次に何か打つと消える。出しっぱなしを防ぐため4秒でも消える）
- **直った直後にBackspaceを押すと、打ったとおりの読みに戻る。** 打ち間違いに気づいた人が
  最初に押すキーがBackspaceなので、そこを取り消しに充てている。訂正のあと何か打っていれば
  Backspaceは普通に1文字消す。取り消した読みは直しにこない
- スペースで変換するほうが休止より早かったときは、読みは書き換えずに候補ウィンドウに
  訂正を足す。打ち間違いのせいで文節の切り方自体が崩れている場合（`さsてえいただいていて` が
  `さ|sて|えいただいていて` に割れる等）は、**文全体を訂正した候補**が先頭の文節に出る。
  この候補を選んで確定しても学習には記録しない
- 「訂正するまでの休止時間」（既定300ms）と「訂正を出す確信の強さ」（既定2.0）を設定で変えられる。
  確信の強さは大きいほど訂正が減り、正しく打った読みを壊すことも減る
- 入力中は「読みの末尾に文字を足すだけ」の訂正はしない。打ちかけの読みは常に終わりが
  足りなく見えるので、モデルが句読点で文を締めようとする（`こえて` → `こえて、`）のを防ぐ
- 未確定のローマ字が残っている間（`k` と打った状態など）は走らない。CPU 1スレッドで平均4ms前後
- 読みが48文字を超えるとき・かな以外の文字が混じるときは何もしない
- モデルはアプリに含まれず、有効にすると約6MBを1回だけダウンロードする（下記「打ち間違い訂正モデルの配布」）

### データの保存場所（複数のMacで共有）

ユーザ辞書・学習・変換ルール・変換モデル・設定は、既定では
`~/Library/Application Support/iroha` に保存される。
**設定 > 情報 > データの保存場所 > 「フォルダを変更...」** でiCloud DriveやDropboxの中の
フォルダ（例: `~/Dropbox/iroha`）を指定すると、複数のMacで同じデータを使える。

- 変更時に今のデータをコピーするか選べる。移行先に既にファイルがあれば上書きしない
  （先に別のMacが置いたデータが優先される）。変更後はirohaが自動で再起動する
- 他のMacからの同期で辞書・学習・ルールのファイルが変わると、再起動なしで読み直す。
  学習は両方のMacの内容をマージする（同じ読みは新しい方を採る）
- 設定（ライブ変換・候補数・AIプリセット・選択テキストのAI処理など）はフォルダ内の
  `settings.json` を介して同期される。保存場所そのもの・モデルの絶対パス・アップデート確認の履歴・
  APIキー（キーチェーン）は端末ごとの値なので同期しない
- 2台で同時に編集した場合は後に保存した方が残る（Dropboxでは「競合コピー」が残ることがある）
- 保存場所を変えていると、アンインストール時の「データも含めて削除」は既定の場所だけを消し、
  共有フォルダには触らない
- フォルダ内の `logs/launch-<ホスト名>.log` に起動・終了の記録が残る（1起動あたり数行のテキスト。
  前回が正常終了していなければ `unclean-exit`、macOSがクラッシュレポートを残していれば
  `crash-report` の行が起動時に書かれる）。irohaが勝手に再起動していないか調べるときに見る

## 開発

### ディレクトリ構成

役割で 4 つに分かれる。リポジトリに入っているのは上の 3 つのコードと評価セットだけで、
学習データ・学習・検証の中身（他者のデータや大きな生成物を含む）は入れていない。

| 役割 | ディレクトリ | Git | 中身 |
|---|---|---|---|
| アプリ | `macos/` | 追跡 | macOS 版（Swift パッケージ。IME 本体・変換エンジン・CLI・追加学習） |
| | `windows/` | 追跡 | Windows 版（TSF） |
| 配布・評価 | `models/` | 追跡 | アプリがダウンロードするモデルの一覧（`typo-normalizer.json`） |
| | `testdata/` | 追跡 | 評価セット（`eval.tsv`、`iroha/typo/`。`ajimee/` は取得して置く） |
| 学習データを作る | `dataset/iroha-typo-normalizer/` | コードだけ追跡 | 公開データから学習データを作る Python パッケージ。作ったデータは `dataset/iroha-typo-normalizer/data/`（追跡しない） |
| 学習・検証 | `training/` | 追跡しない | モデルの学習。直下がかな漢字変換モデル（llm-jp・T5）、`typo-normalizer/` が打ち間違いの訂正 |
| | `experiments/` | 追跡しない | アプリに入れない検証（候補の並べ替え `reranker/`・生成 LM による判定 `jev/`） |
| 外部依存 | `vendor/` `patches/` | パッチだけ追跡 | llama.cpp・辞書データ（取得して置く）と llama.cpp へのパッチ |

データの流れは「`dataset/iroha-typo-normalizer/` で作る → `training/` で学習する → `testdata/` で測る →
`models/`（打ち間違いの訂正）または GGUF で配る → `macos/` `windows/` が使う」。
`training/` の学習スクリプトはルートの `vendor/`・`.venv` を参照するので、これらはルートに置く。

### ビルドとテスト

macOS版のSwiftパッケージは `macos/` 配下にある（Windows版は今後 `windows/` に実装予定）。
ビルドコマンドは `macos/` から実行する:

```sh
cd macos
swift test                                 # 単体テスト（ローマ字変換など）
swift build && .build/debug/iroha-cli repl  # CLIで変換を試す（レイテンシ表示付き）
.build/debug/iroha-cli convert --n 5 "きしゃ"          # n-best候補
.build/debug/iroha-cli convert --context "天気予報によると" "きしょう"  # 文脈条件付け
.build/debug/iroha-cli predict --chain 3 "よろしく"   # 予測（左文脈の続き。--chainでTab連打を模す）
log stream --predicate 'process == "iroha"' --style compact  # IMEのログ
```

#### 打ち間違い訂正モデルの配布

打ち間違いの訂正（上記）のモデルは**アプリに同梱していない**。設定でONにしたときに
アプリが取りにいく（重みは本体コード(MIT)と別ライセンス CC BY-SA 4.0 なので、配布物を分けてある）。

- 一覧: [models/typo-normalizer.json](models/typo-normalizer.json)（このリポジトリのファイル）。
  アプリが焼き込んでいるのは**このURLだけ**なので、モデルの追加・差し替えはカタログの更新だけで済む
- 重み本体: GitHub Releases の専用タグ `typo-normalizer-v1`（アプリのリリース `vX.Y.Z` とは別系列）
- 取得したものは `<データフォルダ>/models/typo-normalizer/` に入る。保存場所を共有フォルダに
  していれば、1回落とすだけで全部のMacで使える
- 落としたあと **SHA-256 と大きさを照合してから設置**する。通らなければ何も置かない
- 置き場所は環境変数 `IROHA_TYPO_MODEL`、カタログのURLは `IROHA_TYPO_CATALOG` で上書きできる

新しいモデルを公開する手順:

```sh
# 1. 書き出しを配布用に整える（float16化 + 移植の照合 + LICENSE/README 添付）
./macos/scripts/publish-typo-normalizer.sh <書き出しディレクトリ> <モデルID>
# 2. 表示された gh コマンドで Release へアップロード（**必ず --prerelease**）
# 3. 表示された JSON を models/typo-normalizer.json に入れて push
# 4. 確認
.build/release/iroha-cli typo catalog          # 一覧が見えるか
.build/release/iroha-cli typo catalog install  # 実際に取得・照合・設置できるか
```

**`--prerelease` を外さないこと。** 通常リリースにすると GitHub の `releases/latest` が
モデルのリリースを指してしまい、アプリの更新通知（[UpdateChecker](macos/Sources/iroha/UpdateChecker.swift)）が壊れる。
なお `release.yml` のトリガは `tags: ['v*']` なので、このタグを push してもアプリのビルドは走らない。

配布するファイルの大きさ（scale-16x の場合）:

| | weights.bin | manifest.json |
|---|---:|---:|
| float16（既定） | 6,397,680 B (6.1 MiB) | 20,807 B |
| float32（`--float32`） | 12,795,360 B (12.2 MiB) | 20,229 B |

float16 でも照合は通る（greedy 200/200 一致・margin 最大差 0.0043、θ別の訂正率の表も float32 と同一）。

##### モデルのライセンス

現在配布しているモデルは **CC BY-SA 4.0**。学習元の
[zenz-v2.5-dataset](https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset)（Keita Miwa 氏）が
CC BY-SA 4.0（一部は llm-jp-corpus-v3 由来で ODC-BY と Common Crawl の規約）なので、継承して同じ条件で配る。
**iroha 本体のコードは MIT のまま**で、重みを別ファイルにしてあるので混ざらない。

ライセンスはカタログの**モデルごと**に書く（アプリに焼き込まない）。学習元を
[dataset/iroha-typo-normalizer/iroha](dataset/iroha-typo-normalizer/) のような別のコーパスに替えたモデルは、条件が変わりうるため。


- 変換エンジンは [ConversionEngine](macos/Sources/IrohaCore/ConversionEngine.swift) プロトコルで抽象化されており、
  [ZenzEngine](macos/Sources/IrohaCore/ZenzEngine.swift)（zenz-v3 + llama.cpp）を別モデルに差し替えられる
- モデルの評価は `iroha-cli bench ../testdata/eval.tsv`（完全一致率・CER・レイテンシ）。
  自作モデルの学習パイプライン（データ準備→学習→GGUF変換→評価）は [training/](training/README.md) を参照
- データフォルダは既定で `~/Library/Application Support/iroha`（設定で変更可。
  [DataDirectory](macos/Sources/IrohaCore/DataDirectory.swift) が唯一の参照点で、以下のパスはすべてここから導く。
  iroha-cliはIME本体の設定に従い、環境変数 `IROHA_DATA_DIR` で上書き可）
- モデルファイルは `<データフォルダ>/models/` に置く（環境変数 `IROHA_MODEL` で上書き可）
- 学習結果は `<データフォルダ>/learning.json`（環境変数 `IROHA_LEARNING` で差し替え可）
- 変換記録は `<データフォルダ>/logs/conversions/`
  （[ConversionLog](macos/Sources/IrohaCore/ConversionLog.swift)。`learning.json` は「次の変換で引く辞書」、
  こちらは「起きたことをそのまま積む追記専用ログ」で役割が違うので別ファイル。
  `ConversionLogEntry.trainingLine` が `training/prepare_data.py` と同じ学習用の1行を返す）
- 追加学習（LoRA）は [macos/Sources/IrohaTrain/](macos/Sources/IrohaTrain/)（MLX Swift）と
  ヘルパー実行ファイル `iroha-train`（`Contents/MacOS/`。設定画面が `Process` で起動し、
  標準出力の JSON Lines = [TrainingEvent](macos/Sources/IrohaCore/TrainingEvent.swift) で進捗を受ける）。
  流れは 記録 → `trainingLine` → ベースモデルの語彙で `llama_tokenize`（[VocabTokenizer](macos/Sources/IrohaCore/VocabTokenizer.swift)）→
  出力部だけに損失 → MLX で LoRA 学習（[GPT2Model](macos/Sources/IrohaTrain/GPT2Model.swift) は llama.cpp の
  `gpt2.cpp` と同じ計算で、ロジットの一致をテストで確認）→ llama.cpp の LoRA アダプタ形式の GGUF に書き出し
  （[LoraAdapterWriter](macos/Sources/IrohaCore/LoraAdapterWriter.swift)）→ `ZenzEngine(adapterPath:)` が
  `llama_adapter_lora_init` で適用。ベースは Q5_K_M のまま（学習用の f16 版は
  `~/Library/Caches/iroha/models-f16/` に [ModelRequantizer](macos/Sources/IrohaCore/ModelRequantizer.swift) が作る）。
  MLX の Metal カーネルは `swift build` では作れないので `macos/scripts/build-mlx-metallib.sh` が
  `.build/release/mlx-swift_Cmlx.bundle/default.metallib` を生成し、make-bundle.sh が Resources へ入れる。
  mlx-swift は 0.31.4 に固定（0.31.5 以降は swift-tools-version 6.3 が必要）。
  CLI では `IROHA_LORA=<adapter.gguf>` でアダプタを適用でき、`scripts/bench-compare.sh model.gguf:adapter.gguf`
  でアダプタ有無を同じ表に並べられる
- 打ち間違いの訂正は [macos/Sources/IrohaCore/TypoNormalizer/](macos/Sources/IrohaCore/TypoNormalizer/)。
  3.2M パラメータの文字単位 Transformer encoder–decoder を Accelerate（`cblas_sgemm`）だけで実装したもので、
  llama.cpp も MLX も通さない。学習と重みの書き出しは `training/typo-normalizer/`。
  移植が正しいかは `iroha-cli typo parity`（PyTorch 実装との照合 200 件）で判定する。
  **`experiments/` と `training/` はリポジトリに含めていない**（学習データが大きく、
  AJIMEE-Bench や学習コーパス由来の実文を含むため。他者のデータを再配布しない方針）。
  下の評価コマンドもその手元のデータを前提にしている。変換エンジンのデコレータ鎖には入れず、
  変換ルールと同じくコントローラが候補ウィンドウを開くときに合流させる
  （読み全体の訂正を、差分が収まっている文節の候補に落とす。[TypoCorrectionPlacement](macos/Sources/IrohaCore/TypoNormalizer/TypoCorrectionPlacement.swift)）

  ```sh
  ./macos/scripts/install-typo-normalizer.sh <書き出しディレクトリ>  # float16に落として vendor/ へ設置 + 照合
  .build/release/iroha-cli typo parity                             # PyTorch実装との照合（200件）
  .build/release/iroha-cli typo eval  ../training/typo-normalizer/data/master/test.jsonl --n 10000
  .build/release/iroha-cli typo bench ../training/typo-normalizer/data/master/test.jsonl --n 300
  .build/release/iroha-cli typo pause    ../training/typo-normalizer/data/master/test.jsonl --n 600
  .build/release/iroha-cli typo prefix   ../training/typo-normalizer/data/master/test.jsonl --n 2000
  .build/release/iroha-cli typo segments ../training/typo-normalizer/data/master/test.jsonl --n 400
  .build/release/iroha-cli typo "をわぇてけいやくする"              # 1件試す（生成・margin・採否）
  ```

  `typo pause` は**人が入力を止めそうな場所（文節の切れ目）で切った読み**への誤検出率で、
  入力中に訂正を走らせる設計の根拠（θ=2.0・末尾への追加を捨てて 1.29%）。
  `typo prefix` は文字数で機械的に切るので語の途中が多く、条件が実際より厳しく出る。
  `typo segments` は訂正が1文節に収まる割合（文全体の訂正候補が要る割合の根拠）

  **計算の順序を変えたら必ず `typo parity` を回すこと**（生成が一致してもロジットがずれていれば実装は間違っている）

- 変換ルールは `<データフォルダ>/user-rewrite-rules.json`
  （[UserRewriteRule](macos/Sources/IrohaCore/UserRewriteRule.swift)。変換エンジンのデコレータ鎖には入れず、
  コントローラが候補ウィンドウを開くときに独立した候補生成源として合流させる。
  トリガーの一致方法は `TriggerKind`、テンプレートへ渡す値は `RewriteMatch.parameters` で拡張する）
- 起動・終了の記録は `<データフォルダ>/logs/launch-<ホスト名>.log`（[LaunchLog](macos/Sources/IrohaCore/LaunchLog.swift)。
  端末ごとにファイルを分けるので共有フォルダでも競合しない。`state-<ホスト名>.json` が直前の起動の記録で、
  起動時に終了時刻が入っていなければ前回は正常終了していない。NSLogはユニファイドログに残らないことがあるため、
  終了の調査はまずこのログと `~/Library/Logs/DiagnosticReports/iroha-*.ips` を見る）
- ユーザ辞書は `<データフォルダ>/user-dictionary.json`（環境変数 `IROHA_USER_DICT` で
  iroha-cli から差し替え可）。macOSのユーザ辞書の実体は `~/Library/KeyboardServices/TextReplacements.db`
  （非公開スキーマのSQLite。実データが未チェックポイントのWALにあるため db/-wal/-shm ごとコピーして読む）
- zenzのプロンプト形式: `[U+EE02 + 左文脈] + U+EE00 + カタカナ読み + U+EE01 → 変換結果`
- 左文脈の取得（[DocumentContextSettings](macos/Sources/iroha/DocumentContextSettings.swift)、設定 `documentContext`、既定ON）:
  合成を始める瞬間（未確定文字列がまだ無いとき）に1回だけ、IMKの `selectedRange` / `attributedSubstring`
  でアプリのカーソル手前のテキストを読み、改行を除いた末尾40文字を文脈にする（[LeftContext](macos/Sources/IrohaCore/LeftContext.swift)）。
  インライン補完は確定直後に読み直す（確定した文字列で終わっていることを確認する）。
  カーソル位置やテキストを返さないアプリ（Electron系・ターミナル等）や設定OFFでは、
  irohaが確定した文字列の蓄積（`recentCommitted`、フォーカス移動で空になる）に代える。
  合成中は読み直さない（未確定文字列が混ざる・同期IPCが増える）
- 予測変換・インライン補完は [PredictionEngine](macos/Sources/IrohaCore/PredictionEngine.swift)
  プロトコルで抽象化し、かな漢字変換とは別モデルを設定できる（既定は同じzenzインスタンスを共有）。
  zenzでの実装（`ZenzEngine.predict`）はazooKey（Zenzai）の次文字予測と同じプロンプト
  `U+EE00 。 U+EE02 + 左文脈` で左文脈の続きを貪欲生成する（zenz-v3は左文脈も学習しているので
  自然な日本語を続ける。左文脈が終わったとみなすと特殊トークンを出すので、それが終端）。
  直前の文字列に出たトークンには近いほど強い繰り返しペナルティをかける（azooKeyと同じ重み）。
  表示する範囲は `PredictionText` が「先頭の1文節（かな→非かなの境界。2文字未満なら2文節目まで）、
  句読点が出たらそこまで、最大16文字」に切り出し、切り出しが決まった時点で生成を止める。
  左文脈は「確定済み文字列（最大40文字）＋表示中の未確定文字列」（予測変換）または
  「確定済み文字列」（インライン補完）。確定済み文字列は下記の左文脈の取得と同じ。コントローラは休止時間（設定 `predictionDelayMs`、既定300ms）を
  キー入力の時刻から測り、ライブ変換の到着後に残り時間だけ待ってから予測を走らせる。
  表示は [CaretPanel](macos/Sources/iroha/CaretPanel.swift)（フォーカスを取らない
  フローティングの `NSPanel`。打ち間違いの訂正の知らせも同じ1枚を使うので、2つが重なって出ることはない）で、位置は `IMKTextInput.attributes(forCharacterIndex:lineHeightRectangle:)`
  が返すカーソル行の矩形の直下。未確定文字列に予測を混ぜない（混ぜると検索欄などが予測文に反応し、
  薄い色の描画もアプリ任せになる）
- 生成は読みで縛る（[ReadingConstraint](macos/Sources/IrohaCore/ReadingConstraint.swift)）。
  ひらがな・句読点は読みと一致する位置でしか出せず、読みを使い切るまで終端させない。
  これがないと「こんにちはあかちゃん → こんにちは。赤ちゃん」のように読みにない文字が混ざる
- 候補ウィンドウの候補は辞書ラティス（[LatticeConverter](macos/Sources/IrohaCore/LatticeConverter.swift)、
  [AzooKeyKanaKanjiConverter](https://github.com/azooKey/AzooKeyKanaKanjiConverter) + azooKey辞書）で作り、
  zenzの対数確率で並べる（[LatticeRescoringEngine](macos/Sources/IrohaCore/LatticeRescoringEngine.swift)）。
  読み制約は漢字・英字の読みを検証できないため、zenzのn-bestだけだと「ないようを → 活用を / NIPPON」の
  ような読みの合わない候補が混ざる。辞書ラティスの候補は読みが保証されるので、それをモデルで順位付けする
  （Zenzaiと同じ役割分担）。zenz自身の生成結果も一緒に採点するので、辞書にない語も候補に残る。
  候補ウィンドウには「zenzで並べた上位（候補数の設定）」の後ろに「読みが一致する残りの辞書エントリ」
  （単漢字・異体字・人名など）が辞書の順で続き、スクロールで辿れる（azooKeyの候補一覧と同じ）。
  さらに髙・﨑・德・濵のように単独では辞書に入っていない人名用の異体字を
  [VariantKanjiEngine](macos/Sources/IrohaCore/VariantKanjiEngine.swift) の内蔵表から末尾に補う。
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

## コントリビューター

- [shiratsumu](https://github.com/shiratsumu) — ユーザフィードバック

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
