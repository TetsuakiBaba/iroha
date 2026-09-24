# データソースとライセンス

このディレクトリの **コードは MIT License**（`LICENSE`）。
**データは別**で、ソースごとに条件が違う。ここがその一覧。

## 派生データの公開可否（重要）

**生成したデータ（`data/` 以下）は Git 管理せず、配布もしない。**
各自が元データからスクリプトで再生成する方式を維持する。理由:

- Tatoeba は CC BY 2.0 FR で、**文ごとに投稿者が違う**。派生物を再配布するなら
  文ごとの帰属表示が必要になる。canonical record には `attribution` を入れてあるので
  技術的には出せるが、その形で配って条件を満たせるかの判断は別途必要
- KAKEN は CC BY 4.0 互換だが、**出典表記と「加工したことの明示」が必須**。
  さらに「編集・加工した情報を、あたかも国立情報学研究所が作成したかのような態様で
  公表・利用してはいけない」という条件がある
- 2015年度以前の採択課題の報告書には「著作物であることを明示されている部分」が
  含まれうる（下記）。安全側に切っているが、切り方の妥当性を外部に対して
  保証できるわけではない

したがって `.gitignore` で `data/` を除外している。この方針を変えるときは、
ソースごとの条件を再確認してから変えること。

---

## A. Tatoeba（日本語文）

| 項目 | 内容 |
|---|---|
| ソース名 | Tatoeba Project — Japanese sentences |
| URL | https://tatoeba.org/ |
| 配布元 | https://downloads.tatoeba.org/exports/ （**一次配布元。HF のミラーは使わない**） |
| ライセンス | **CC BY 2.0 FR**（一部の文は **CC0 1.0**） |
| ライセンス本文 | https://creativecommons.org/licenses/by/2.0/fr/ |

### 取得ファイル

| ファイル | 使うもの |
|---|---|
| `per_language/jpn/jpn_sentences.tsv.bz2` | `id`, `text` |
| `per_language/jpn/jpn_sentences_detailed.tsv.bz2` | `username`（出典表記用） |
| `per_language/jpn/jpn_sentences_CC0.tsv.bz2` | CC0 の文の一覧（record ごとに license を分ける） |
| `jpn_indices.tar.bz2` | 田中コーパス由来の語＋読み注記（読みの second opinion） |

### attribution 方法

canonical record の `attribution` に文ごとの表記を入れている。

```
Tatoeba sentence #<id> by <username> (CC BY 2.0 FR)
```

データベース全体に対しては:

```
出典: Tatoeba Project (https://tatoeba.org/) の日本語文。CC BY 2.0 FR
```

### 注意事項

- 文の大半は CC BY 2.0 FR。`jpn_sentences_CC0.tsv` に載っている文だけ CC0 1.0。
  日本語の CC0 文は現時点でごくわずか（実測 2 文）なので、実質すべて CC BY と考える
- 投稿者による例文なので **1 文 = 1 document**。文をまたぐ左文脈は作れない
  （文内の確定文字列だけが context になる）
- 会話体・短文が多い。学術語彙・長文は KAKEN 側で補う
- `jpn_indices` は田中コーパス（Tanaka Corpus）由来。読み注記が付いている語だけが
  second opinion の対象なので網羅はしないが、同形異音語（二十歳・一日 など）に効く

---

## B. KAKEN（科学研究費助成事業データベース）

| 項目 | 内容 |
|---|---|
| ソース名 | KAKEN：科学研究費助成事業データベース（国立情報学研究所） |
| URL | https://kaken.nii.ac.jp/ |
| 利用規程 | https://support.nii.ac.jp/ja/kaken/about/terms |
| ライセンス | **CC BY 4.0 と互換**（「文部科学省ウェブサイト利用規約」準拠） |

利用規程の該当箇所（要旨）:

> KAKENで公開されるコンテンツの利用については、一部報告書等の作成者自身が著作権を持つ
> コンテンツを除き、「文部科学省ウェブサイト利用規約」に準拠しています。（…）
> 本利用ルールが適用されるコンテンツはCC BYに従うことでもコンテンツを利用することができます。
>
> KAKENで公開されるコンテンツのうち、著作物であることを明示されている部分
> （**2015年（平成27年度）以前の採択課題に関して提出された報告書等の一部**）については、
> 「国立情報学研究所学術コンテンツサービス利用規程」（学術情報等の著作権）第５条第２項に
> 準じてご利用ください。

### 使用フィールド

| XPath | 内容 |
|---|---|
| `grantAward/summary[@xml:lang="ja"]/paragraphList/paragraph` | 研究概要・研究成果の概要・社会的意義 |
| `grantAward/reportList/report[@xml:lang="ja"]/paragraphList/paragraph` | 報告書本文（`fields` で種別を選ぶ） |
| `grantAward/summary/title` | 出典表記用 |
| `grantAward/@awardNumber` | 出典表記用 |
| `grantAward/summary/periodOfAward/startDate` | **採択年度の判定用** |

### ライセンス上の安全側フィルタ（実装済み）

1. **採択年度 2016 以降の課題だけを使う**（`sources.kaken.min_award_year`、既定 2016）。
   2015年度以前は「著作物であることを明示されている部分」が含まれうるため
2. 採択年度は `periodOfAward/startDate` から取る。**取れない課題は使わない**
   （課題番号の年号部分は旧形式が和暦・新形式が西暦で解釈が揺れるため、判定に使わない）
3. 念のため、報告書の `@fiscalYear` が `min_award_year` より前のものも使わない

### attribution 方法

**出典表記は必須。加工したことの明示も必須。** canonical record の `attribution` に
課題ごとの表記を入れている。

```
出典：「<研究課題名>」課題番号<課題番号>（KAKEN：科学研究費助成事業データベース（国立情報学研究所））（ <URL> ）を加工して作成
```

データベース全体に対しては:

```
出典：KAKEN：科学研究費助成事業データベース（国立情報学研究所）（ https://kaken.nii.ac.jp/ ）をもとに iroha-dataset が加工して作成
```

### 注意事項

- **「あたかも国立情報学研究所が作成したかのような態様で公表・利用してはいけない」**
  （利用規程）。学習済みモデルや派生データの説明でもここを守る
- 報告書等については、申し出により作成者自身が著作権を主張できる場合がある
  （コンテンツ公開オプトアウト）。オプトアウトされたものは公開 XML に載らないが、
  取得時点のスナップショットを使い続けると反映されない。**定期的に取り直すこと**
- 検索 API（`mode: search`）は **appid の登録が必要**
  （https://support.nii.ac.jp/ja/cinii/api/developer で登録し、環境変数 `KAKEN_APPID` に入れる）。
  appid なしで動かす `mode: ids` は課題ごとの公開 XML を 1 件ずつ取る
- 課題ごとの XML は、存在しない課題番号でも **HTTP 200 + 空の本文** で返る
  （404 ではない）。`mode: ids` はこれを「その番号の課題は無い」として数えている
- 公開サーバなので `request_interval` を **1 秒以上**空ける（既定 1.0 秒）。
  `mode: ids` は候補の課題番号を機械的に作るので空振りが出る（実測でだいたい半分）
- **検索 API（opensearch）は throttle が厳しい。** 実測（2026-09-22）で、20 分ほど
  短間隔（1〜20 秒）で試したところ throttle に入り、2 分冷却 + 30 秒間隔でも回復しなかった。
  throttle 中は `totalResults=0` か `403 Invalid APPID` が返る（どちらも
  「データが無い」「appid が無効」ではない）。NII の規約は過度なアクセスでの
  アクセス制限・登録取消に言及しているので、**探索的に叩かず `request_interval` を
  10 秒以上にすること**
- データベースには誤りが含まれうる（利用規程に明記されている）

---

## C. 青空文庫（既定 OFF / 将来用）

| 項目 | 内容 |
|---|---|
| ソース名 | 青空文庫（著作権消滅作品） |
| URL | https://www.aozora.gr.jp/ |
| ライセンス | 著作権消滅（パブリックドメイン） |
| 収録基準 | https://www.aozora.gr.jp/guide/kijyunn.html |

### 使用フィールド

- 作品本文（ルビ `《》`・ルビ始点 `｜`・入力者注 `［＃…］` を除去）
- 一覧 CSV（`list_person_all_extended_utf8.zip`）の `作品著作権フラグ` / `文字遣い種別`

### 注意事項

- **既定 OFF。** 有効にする前に `data/samples/canonical_samples.txt` を目視すること
- `作品著作権フラグ = "なし"`（= 著作権消滅）の作品だけを使う。青空文庫には
  著作権者の許諾を得て公開している作品もある（フラグ `あり`）ので、それは使わない
- **3 作品での動作は確認済み・大規模では未検証**
- **`文字遣い種別 = "新字新仮名"` の作品だけを使う**（`allowed_orthography`）。
  旧字旧仮名は読みが現代かなと合わないので、Sudachi の読みをそのまま教師にすると壊れる
- 青空文庫の記法は作品ごとに揺れる。大規模に使う前に目視が必要
- 入力者・校正者のクレジットは作品ファイル末尾にある。出典として残すこと
- 文学作品なので、iroha の用途（現代の実務的な入力）とは語彙・文体が離れている

---

## D. JWTD（日本語 Wikipedia 入力誤りデータセット v2.0）— `build-jwtd` 専用

ソースアダプタではない（`download` / `preprocess` の対象外）。手元に置いた配布物を
`build-jwtd` が読み、typo normalizer 用の学習データとベンチマークを作る。

| 項目 | 内容 |
|---|---|
| ソース名 | 日本語Wikipedia入力誤りデータセット（Japanese Wikipedia Typo Dataset, JWTD）v2.0 |
| URL | https://nlp.ist.i.kyoto-u.ac.jp/?日本語Wikipedia入力誤りデータセット |
| 配布元 | https://nlp.ist.i.kyoto-u.ac.jp/nl-resource/JWTD/jwtd_v2.0.tar.gz （京都大学 言語メディア研究室） |
| ライセンス | **CC BY-SA 3.0**（元データである日本語 Wikipedia に従う、と配布ページに明記） |
| ライセンス本文 | https://creativecommons.org/licenses/by-sa/3.0/ |

### 使用フィールド

`train.jsonl` / `test.jsonl` / `gold.jsonl` の `page`・`title`・`pre_rev`・`post_rev`・
`pre_text`・`post_text`・`diffs[].pre_str / post_str / category`。
尤度（`*_likelihood`）は使わない。

### attribution 方法

配布ページが引用を求めている文献:

```
田中 佑, 村脇 有吾, 河原 大輔, 黒橋 禎夫: 日本語Wikipediaの編集履歴に基づく入力誤りデータセットと
訂正システムの改良, 言語処理学会 第27回年次大会, 2021.
```

データ全体に対しては:

```
出典: 日本語Wikipedia入力誤りデータセット v2.0（京都大学 言語メディア研究室）を加工して作成。
元データは Wikipedia 日本語版（CC BY-SA 3.0）
```

### 注意事項

- **SA（継承）が付く。** JWTD から作ったデータで学習したモデルの重みを配るときは、
  CC BY-SA 3.0 か、同じ要素を持つ後の版（BY-SA 4.0）で出す。typo normalizer の重みは
  もともと CC BY-SA 4.0 なので条件は合うが、**公開するモデルのカタログ
  （`models/typo-normalizer.json`）の `attribution` に JWTD と Wikipedia を足すこと**
  （ライセンスはアプリに焼き込まず、カタログのモデルごとに持つ方針）
- 生成物（`data/jwtd/`）は他のソースと同じく Git に入れず、配布もしない
- 本文は Wikipedia の版であり、執筆者ごとの帰属は版の履歴（`page` と `pre_rev` / `post_rev`）
  でたどれる。生成物の各行にはこの 3 つを残してある

---

## E. LLM-jp Corpus v4（ja_kaken / ja_e-gov / ja_patent / ja_aozorabunko）— typo normalizer 用コーパス

`config/typo-corpus.yaml` で有効にする（既定 OFF）。アダプタは `iroha/sources/llmjp.py`。

| 項目 | 内容 |
|---|---|
| ソース名 | LLM-jp Corpus v4（LLM-jp コーパス構築 WG） |
| URL | https://gitlab.llm-jp.nii.ac.jp/datasets/llm-jp-corpus-v4 |
| ライセンス | 4 サブコーパスとも **CC BY 4.0**（README-ja.md の「各サブコーパスの詳細・ライセンス」） |
| ライセンス本文 | https://creativecommons.org/licenses/by/4.0/ |

| アダプタ | サブコーパス | 中身・一次配布先 |
|---|---|---|
| `llmjp_kaken` | `ja/ja_kaken` | KAKEN の研究課題の概要 |
| `llmjp_egov` | `ja/ja_e-gov` | e-Gov 法令（一次配布 https://huggingface.co/datasets/nlp-waseda/e_gov） |
| `llmjp_patent` | `ja/ja_patent` | 特許庁の公報データファイルから抽出した公報 |
| `llmjp_aozora` | `ja/ja_aozorabunko` | 青空文庫（一次配布 https://huggingface.co/datasets/globis-university/aozorabunko-clean） |

### 使用フィールド

`text`。`meta` はサブコーパスごとの選別と出典表記にだけ使う（e-Gov の `LawNum`、
青空文庫の `文字遣い種別` / `作品著作権フラグ` / `作品名` / `姓` / `名`）。

### attribution 方法

```
出典: LLM-jp Corpus v4（https://gitlab.llm-jp.nii.ac.jp/datasets/llm-jp-corpus-v4）ja/<サブコーパス>、
LLM-jp コーパス構築 WG、CC BY 4.0。文への分割・読みの付与など加工して作成
```

canonical record の `attribution` に文書ごとに入る（e-Gov は法令番号、青空文庫は作品名・著者名を足す）。

### 注意事項

- LLM-jp は**日本国著作権法を適用するため日本国内のサーバから配布**しており、
  国外のサーバから再配布すると同法が適用されない旨を README に書いている。
  生データ・生成物とも再配布しない（この文書冒頭の方針どおり）
- 各文書の著作権は原則として著作者に帰属する（LLM-jp の README）
- `ja_kaken` は自前の `kaken` ソース（KAKEN API）と中身が重なる。同時に有効にしない
- `ja_e-gov` は、ひらがなを含まない段落（カタカナ文語の旧法令）を捨てる
- `ja_aozorabunko` は `作品著作権フラグ = なし` かつ `文字遣い種別 = 新字新仮名` の作品だけを使う
- `ja_patent` は 68B トークンあるので、621 ファイルから等間隔に選んだ本数だけを使う

---

## F. zenz-v2.5-dataset / train_wikipedia.jsonl — typo normalizer 用コーパス

`config/typo-corpus.yaml` で有効にする（既定 OFF）。アダプタは `iroha/sources/zenz_wiki.py`。

| 項目 | 内容 |
|---|---|
| ソース名 | zenz-v2.5-dataset（Keita Miwa）の Wikipedia サブセット |
| URL | https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset |
| ライセンス | **CC BY-SA 4.0**（2024 年 2 月取得の Wikipedia 日本語版アーカイブが元、とデータセットカードに明記） |
| ライセンス本文 | https://creativecommons.org/licenses/by-sa/4.0/deed.ja |

### 使用フィールド

`train_wikipedia.jsonl` の `output`（表層）だけ。`input`（読み）は使わず、Sudachi で付け直す。
`train_llm-jp-corpus-v3.jsonl`（Common Crawl 由来、ODC-BY + Common Crawl 規約）は**使わない**。

### attribution 方法

```
出典: zenz-v2.5-dataset（Keita Miwa、https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset）の
train_wikipedia.jsonl（Wikipedia 日本語版、CC BY-SA 4.0）。読みを付け直すなど加工して作成
```

### 注意事項

- **SA（継承）が付く。** このソースを混ぜた生成物と、それで学習したモデルの重みは
  CC BY-SA 4.0 で扱う。typo normalizer の重みはもともと CC BY-SA 4.0 なので条件は合うが、
  **公開するモデルのカタログ（`models/typo-normalizer.json`）の `attribution` に
  zenz-v2.5-dataset・Wikipedia と LLM-jp Corpus v4 を書くこと**
- JWTD のベンチ（これも Wikipedia 由来）と表層が 12 字以上重なる行は捨てる（評価の漏れ止め）

---

## G. 将来の追加候補

`iroha/sources/base.py` の `SourceAdapter` を実装すれば足せる（手順はその docstring）。
**追加するときは必ずこのファイルに節を足す**（`tests/test_config_cli.py` が
ソースごとのライセンス情報の有無を検査している）。

条件が明確で候補になりうるもの:

| 候補 | ライセンス | 注意 |
|---|---|---|
| Wikidata の日本語ラベル・説明 | CC0 1.0 | 文ではなく語句が多い。KKC の語彙補強向き |
| Apache 2.0 / CC BY の日本語合成コーパス | 各々 | 合成なので文体の偏りを確認してから使う |
| CC BY-SA の日本語コーパス | CC BY-SA | **派生物が SA に縛られる**。学習データに混ぜる影響を先に判断すること |

避けるもの: ライセンス不明、NC（非商用限定）、クロール由来で権利が整理されていないもの。

---

## 使っていないもの

- **`zenz-v2.5-dataset`**: iroha はこれまで zenz 系の学習データを使ってきたが、
  かな漢字変換用・既定の typo 用のパイプラインは**依存しない**（自前で再生成できることが目的）。
  例外は typo normalizer 用コーパス（`config/typo-corpus.yaml`）の `train_wikipedia.jsonl` だけ（F 節）。
  `train_llm-jp-corpus-v3.jsonl` はどこでも使わない
- **Hugging Face 上のミラー**: 一次配布元から取る（更新のタイミングと
  ライセンス表記が一次側と食い違うことがあるため）
