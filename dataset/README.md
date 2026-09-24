# iroha-dataset

ディレクトリは `dataset/`、Python パッケージは `dataset/iroha/`（`python -m iroha …`）。
名前の「iroha-dataset」は pip のパッケージ名・`--version`・出典表記に使っている（2026-09-24 に `iroha-dataset/iroha_dataset/` から改名）。

> **このディレクトリの役割: 学習データを作る。** コード（Python パッケージ）は Git で追跡し、作ったデータは
> `data/`（追跡しない）に置く。作ったデータで学習するのは `../training/`、評価セットのうち追跡するものは `../testdata/`。
> 全体の構成はルートの README.md の「ディレクトリ構成」節。

[iroha](../README.md) 日本語入力システム用の学習データセットを、**ライセンスが明確な公開データから
自前で再生成する**パイプライン。作るのは 2 種類。

1. **かな漢字変換用データ** — 左の確定文字列 + 入力中のかな → 変換結果
2. **typo normalizer 用データ** — 崩れたかな → 意図された正しいかな

以降、かな漢字変換を **KKC**（Kana-Kanji Conversion）と略す。
`data/kkc/` と `build-kkc` コマンドの `kkc` はこれ。

`zenz-v2.5-dataset` などには依存しない。ダウンロードから学習データ生成までスクリプトで
完全に再現できる。

## 設計方針

**「大量であること」より「教師ラベルの品質・由来・再現性」を優先する。**

- **Sudachi の読みを 100% 正解とは仮定しない。** 形態素ごとに surface / reading /
  normalized_form / POS / OOV を残し、怪しいものには `reading_confidence: "low"` を立てる。
  読みがひらがなに落ちない文は残さず捨てる
- 品質に疑問があるサンプルを無理に残すより**捨てる**（何をなぜ捨てたかは `data/REPORT.md` に出る）
- **生成データは Git に入れない。** 各自が元データから再生成する（→ [LICENSES.md](LICENSES.md)）
- 同じ seed・同じ設定なら同じデータになる（`tests/test_pipeline_end_to_end.py` で検査）

## 準備

```sh
cd dataset
python3 -m venv .venv
./.venv/bin/python -m pip install -e ".[dev]"
```

SudachiDict-full は 120MB ほどある。`reading.dictionary` を `core` にすれば
`sudachidict-core` でも動くが、**既定は full**（専門用語の読みが取れるかどうかが
KAKEN のデータで効くため）。

## 実行

```sh
cd dataset

./.venv/bin/python -m iroha download      # 公開データを data/raw/ に取る
./.venv/bin/python -m iroha preprocess    # 正規化 → 読み → フィルタ → canonical
./.venv/bin/python -m iroha build-kkc     # かな漢字変換用データ
./.venv/bin/python -m iroha build-typo    # typo normalizer 用データ
./.venv/bin/python -m iroha build-readings  # typo を付けない正しい読みの一覧（オンザフライ学習用）
./.venv/bin/python -m iroha build-jwtd    # 実 typo（JWTD）→ 学習データ + ベンチ（要 --set jwtd.src=…）
./.venv/bin/python -m iroha estimate-typo-dist  # JWTD の実誤りから typo 生成器の抽出確率を推定（build-jwtd の後）
./.venv/bin/python -m iroha samples       # 目視確認用のランダム抽出
./.venv/bin/python -m iroha stats         # stats.json と REPORT.md
```

一括:

```sh
./.venv/bin/python -m iroha build-all
```

小規模の動作確認（Tatoeba 1000 文 + KAKEN 60 課題、`data-smoke/` に出る）:

```sh
./.venv/bin/python -m iroha build-all --config config/smoke.yaml
```

ソースとライセンスの確認:

```sh
./.venv/bin/python -m iroha sources
```

### 共通オプション

| オプション | 意味 |
|---|---|
| `--config config/smoke.yaml` | 設定ファイル（`config/default.yaml` に重ねる） |
| `--set typo.clean_ratio=0.3` | 個別の上書き（値は YAML として解釈。複数指定可） |
| `--source tatoeba` | 対象ソースを絞る（複数指定可） |
| `--force` | `download` で既存ファイルを取り直す |

### KAKEN の取得について

既定の `mode: ids` は **appid 不要**で、課題ごとの公開 XML を 1 件ずつ取る。
課題番号を機械的に作って当たったものを保存するので、空振りが半分ほど出る
（1 件 1 秒なので、300 課題で 10 分ほど）。`sources.kaken.max_projects` で件数を決める。

課題番号のリストが手元にあるなら、そのほうが速くて礼儀もよい:

```yaml
sources:
  kaken:
    id_file: my-award-numbers.txt   # 1 行 1 課題番号
```

### 検索 API（`mode: search`）

appid の登録が必要（https://api.ci.nii.ac.jp/ja/ ）。**appid はリポジトリに書かず環境変数で渡す。**

```sh
export KAKEN_APPID=xxxxxxxx
./.venv/bin/python -m iroha download --source kaken \
  --set sources.kaken.mode=search \
  --set sources.kaken.max_projects=2000 \
  --set sources.kaken.request_interval=10
```

応答は**課題ごとの XML と同じスキーマで、研究概要の本文段落がそのまま入る**
（`rw=500` で 1 リクエスト 500 課題）。これが `ids` モードとの決定的な差で、
理屈のうえでは 12 万課題が 240 リクエストほどで取れる。

#### 実測で分かった癖（2026-09-22）

**必ず読むこと。** ここを知らないと空のデータセットを作ってしまう。

| 挙動 | 対処 |
|---|---|
| **throttle されると `totalResults=0` を HTTP 200 で黙って返す**（エラーにならない） | 0 件が `search.max_consecutive_empty` 回続いたら例外にして止める |
| throttle 中は `403 <detail>Invalid APPID</detail>` が返ることもある（appid が正しくても） | メッセージだけで appid の無効を判断しない。時間をおいて試す |
| throttle の窓は**長い**。実測で 20 分ほどの短間隔アクセスのあと、2 分冷却 + 30 秒間隔 6 回でも回復せず | `request_interval` を **10 秒以上**にして、少ない `max_projects` から始める |
| `kw` は**必須**。`s1`（助成期間）や `qc`（研究種目）だけでは `totalResults=0` | `search.keywords` に分野横断の語を並べて振る（重複は課題番号で落ちる） |
| `kw=*` はワイルドカードではなく**文字通り「*」**で検索される（実測 24 件） | 使わない |
| 1 課題あたり平均 83KB（`productList` が大きい） | `store_trimmed_xml: true`（既定）で使うフィールドだけに削る。**96% 減、3.7KB/課題** |

短間隔で API を試すと簡単に throttle に入る。**探索的に叩かないこと。**
検索経路の検証は `tests/test_kaken_search.py` が保存済みの応答を差し込んで行うので、
コードを直すだけなら API は不要。

### 保存形式（`store`）

| 値 | 中身 |
|---|---|
| `jsonl`（既定） | `data/raw/kaken/projects/<年度>.jsonl` に 1 行 1 課題で追記 |
| `xml` | `data/raw/kaken/xml/<課題番号>.xml` に 1 課題 1 ファイル |

`documents()` は**両方読む**ので、途中で変えても取得済みは無駄にならない。
既定を JSONL にしてあるのは、11 万課題を 1 課題 1 ファイルで置くと
**11 万ファイル**になり、同期フォルダ（Dropbox 等）に置くと辛いため。
実測では 2,323 課題で **26MB / 2,323 ファイル → 2.5MB / 11 ファイル**になった。

すでに XML で取ってあるものは、まとめ直せる:

```sh
./.venv/bin/python scripts/migrate-kaken-xml-to-jsonl.py            # JSONL に追記（XML は残る）
./.venv/bin/python scripts/migrate-kaken-xml-to-jsonl.py --delete-xml
```

### `max_projects` と `max_documents`

| 設定 | 対象 |
|---|---|
| `sources.kaken.max_projects` | **download** で集める課題数の目標 |
| `sources.kaken.max_documents` | **preprocess** で読む課題数の上限（0 = キャッシュ全部） |

兼用にしていたときは、キャッシュに 323 課題あるのに `max_projects: 200` のせいで
200 課題しか使われない事故が起きた。別にしてある。

## 出力

```
data/
  raw/                     ダウンロードしたまま（raw_dir。data_dir と分けてある）
  canonical/
    <source>.jsonl             1 文 1 行の共通形式
    <source>.morphemes.jsonl   形態素ごとの surface/reading/POS/OOV
  kkc/       train.jsonl / validation.jsonl / test.jsonl
  typo/      train.jsonl / validation.jsonl / test.jsonl
  samples/   canonical_samples.txt / kkc_samples.txt / typo_samples.txt
  stats.json
  REPORT.md
```

### canonical record

```json
{
  "id": "kaken_16H03511_0000",
  "source": "kaken",
  "document_id": "kaken_16H03511",
  "sentence_index": 0,
  "text": "本研究では新しい入力手法を提案する。",
  "reading": "ほんけんきゅうではあたらしいにゅうりょくしゅほうをていあんする",
  "previous_text": "視覚障害者の情報アクセスについて検討した。",
  "license": "CC BY 4.0 互換（文部科学省ウェブサイト利用規約準拠）",
  "attribution": "出典：「…」課題番号16H03511（KAKEN…）を加工して作成",
  "has_oov": false,
  "reading_confidence": "high",
  "confidence_reasons": [],
  "chunks": [
    {"target": "本研究では", "reading": "ほんけんきゅうでは"},
    {"target": "新しい入力手法を", "reading": "あたらしいにゅうりょくしゅほうを"},
    {"target": "提案する", "reading": "ていあんする"}
  ],
  "split": "train"
}
```

`reading` は**文末の句点を含めない**（IME では句点を打たずに確定することも多い）。
`chunks` を繋ぐと `text`（末尾の句点を除く）と `reading` に戻る。

### kkc example

```json
{"id":"…","source":"kaken","context":"本研究では","input":"あたらしいにゅうりょくしゅほうを","target":"新しい入力手法を"}
```

1 文から文節チャンクの数だけ example ができる:

```json
{"context":"","input":"ほんけんきゅうでは","target":"本研究では"}
{"context":"本研究では","input":"あたらしいにゅうりょくしゅほうを","target":"新しい入力手法を"}
{"context":"本研究では新しい入力手法を","input":"ていあんする","target":"提案する"}
```

KAKEN のように段落内で文が続くソースでは、前の文も context に入る:

```json
{"context":"視覚障害者の情報アクセスについて検討した。本研究では","input":"あたらしい…","target":"新しい…"}
```

context は **右端（カーソル直前）を残して** `context.max_context_chars` 文字に切る。

### typo example

```json
{"id":"…","source":"kaken","input":"あたらしいにゅうりょkしゅほうを","target":"あたらしいにゅうりょくしゅほうを","error_type":"deletion"}
```

## 仕組み

### 読み生成と品質フィルタ

`reading.mode: C`（長単位）で読みを作る。複合語の読みが正確なため
（`東京都立大学` → `とうきょうとりつだいがく`。mode A だと 3 語に分かれる）。

**捨てる**（`data/REPORT.md` の「フィルタで捨てた文」「読み生成の失敗」に理由別の件数が出る）:

| 条件 | 設定 |
|---|---|
| OOV を含む | `reading.reject_oov`（既定 true） |
| 読みが取れない語を含む | 常に |
| 読みがひらがなに落ちない（ラテン文字・数字が残る） | 常に |
| `reading.allow_extra_chars` 以外の記号・空白を含む（`()` `『』` `【】` `/` 全角空白など） | 常に |
| 制御文字・壊れた Unicode | 常に |
| HTML 断片・URL | `filter.reject_html` / `filter.reject_urls` |
| 数式・記号だけ | `filter.min_content_chars` / `filter.max_symbol_ratio` |
| 極端に長い / 短い | `filter.max_chars` / `filter.min_chars` |
| ラテン文字を含む | `filter.max_latin_run`（既定 0 = 許さない） |
| 数字を含む | `filter.allow_digits`（既定 false） |

数字を既定で捨てているのは、**数の読みが当てられない**ため（`3日` は「みっか」だが
Sudachi は文脈次第で外す）。`filter.allow_digits: true` にすれば通るが、
その場合は `data/samples/canonical_samples.txt` で読みを確かめてから使うこと。

記号・空白の読みは**表層のまま**にする。Sudachi は `()` `『』` `〜` `/` `♪` 全角空白などに
読み「キゴウ」を返すので、それを採ると `司法全般(警察` が `しほうぜんぱんきごうけいさつ` になる
（2026-09-23 に修正。それ以前に作った KAKEN の canonical の 5.57%、Tatoeba の 0.13% に混入していた）。
表層が許可文字（`reading.allow_extra_chars`）でない記号を含む文は、読みがひらがなに落ちないものとして捨てる。

**低信頼フラグ**（捨てずに `reading_confidence: "low"` を立てる。`reading.low_confidence` で調整）:

| 理由 | 内容 |
|---|---|
| `mode_disagreement` | 別の分割単位（mode A）で作った読みと一致しない |
| `proper_noun` | 人名・地名・固有名詞（Sudachi の読みが当たらないことがある） |
| `numeral` | 数詞 |
| `surface_fallback` | 漢字を含む表層なのに読みが表層のまま（辞書に読みが無い） |
| `long_reading` | 読み長 / 表層長 が `max_reading_surface_ratio` を超える |
| `indices_disagreement` | Tatoeba の `jpn_indices`（田中コーパス由来の読み注記）と一致しない |

人名・地名をすべて捨てはしない（捨てると固有名詞が一切変換できないモデルになる）。
`kkc.skip_low_confidence` / `reading.drop_low_confidence` で捨てる運用にもできる。

**別解析器との一致確認を足す**には `iroha/preprocess/reading.py` の
`SecondOpinion` プロトコル（`check(text, result, meta) -> list[str]`）を実装して
`ReadingAnalyzer(cfg, second_opinions=[...])` に渡す。同梱のものは Sudachi の
別分割（`ModeSecondOpinion`）と Tatoeba の `jpn_indices`（`IndicesSecondOpinion`）。

### 文節チャンク

形態素そのままでは細かすぎる（`新しい` `入力` `手法を`）ので、**助詞まで含めた
自然な IME 入力単位**にまとめる（`iroha/preprocess/chunking.py`）:

1. 自立語で新しい文節を始める。助詞・助動詞・接尾辞・非自立語・記号は前にくっつける。
   接頭辞は後ろにくっつける（`本` + `研究` → `本研究`）。
   助詞の直後のかなだけの用言も前にくっつける（`に` + `つい` + `て` → `について`）
2. 助詞・助動詞で終わっていない文節（連体修飾・裸の名詞）を次の文節に合流させる
   （`chunk.merge_modifiers`）。`新しい` + `入力手法を` → `新しい入力手法を`
3. `chunk.min_chars` に届かない文節を寄せる
4. `chunk.max_chars` を超える文節を形態素境界で割る

### typo 生成

**かな文字をランダムに消すのではなく**、必ず

```
正しいひらがな → ローマ字（打鍵列）→ 疑似キー入力エラー → かなへ再変換
```

の順で作る。打鍵列 → かなは `macos/Sources/IrohaCore/RomajiComposer.swift` の移植
（`iroha/typo/romanize.py`）なので、生成されるかなは**実際の iroha が
その打鍵列に対して出すもの**と一致する。解決できない打鍵がラテン文字のまま残るのも
iroha の挙動どおり（`ていsでい` のような input が出るのは正しい）。

隣接キーは Unicode ではなく **QWERTY の物理座標**から作る（`typo/keyboard.py`。
段の横ずれは数字段 0・上段 0・中段 0.25・下段 0.75 キー分）。

| error_type | 内容 |
|---|---|
| `deletion` | キーの押し損ね（位置は既定ではどの打鍵も同じ確率。`typo.key_weights` で重み付けできる） |
| `insertion` | 余分なキー入力（隣接キーが割り込んだ形） |
| `substitution` | QWERTY 上で隣接したキーへの誤入力 |
| `transposition` | 隣接する打鍵の順序逆転 |
| `repeated_key` | キーを余分に複数回入力（既定では音節頭の子音を優先＝促音になりやすい） |
| `missing_double_consonant` | 促音の二重子音の一方が欠ける（`kitte` → `kite`） |
| `excessive_double_consonant` | 促音の子音が余分（`kitte` → `kittte`） |
| `mixed_input` | IME 切替のし忘れ（`きょうはdaigaku`）。**既定では生成しない**（ローマ字の打ち間違いではなく IME の状態の問題なので）。`typo.mixed_input.enabled` で ON/OFF |
| `weak_finger_omission` | 小指・薬指の担当キーが押し切れず落ちる |

- 1 サンプルの typo は既定 1 個、`typo.second_error_ratio` の割合で 2 個
  （上限 `typo.max_errors_per_sample`）
- **かなが変わらない崩しは typo ではないので引き直す**（`shi` の `h` が落ちて `si` → `し`）。
  2 つ目の typo が 1 つ目を打ち消して元に戻る場合も弾く
- `typo.romaji_style` で「ん」の打ち方の個人差を混ぜる（常に `nn` / 子音の前だけ `n`）
- `typo.units` で生成単位を選ぶ（既定 `[sentence, chunk]`）。実際の iroha は
  文節くらいの長さで訂正を走らせるので、チャンク単位のほうが本番に近い

#### error type の比率

既定の比率は、ハードウェアキーボードの打ち間違いを分類した実測
（Komatsu & Nakatoh, "Analysis of Mistyping in Hardware Keyboard", ICISIP 2018,
DOI [10.12792/icisip2018.079](https://doi.org/10.12792/icisip2018.079)。20 代 12 名が
ひらがなの単語を 1,000 語ずつローマ字入力）の全被験者の内訳（Fig. 3）に合わせてある。

| 論文の分類 | 比率 | 生成器の型 |
|---|---:|---|
| Replacement | 60.3% | `substitution` |
| Insertion / Involvement（隣のキーも押す） | 12.5% | `insertion`（隣接キー） |
| Insertion / Other | 4.2% | `insertion`（離れたキー。`typo.dist.insertion_far_ratio: 0.251`） |
| Insertion / Repetition | 9.4% | `repeated_key` |
| Removal | 11.5% | `deletion` |
| Exchange | 2.2% | `transposition` |

論文の分類に無い型は 0 にしてある。促音の過不足（`missing_double_consonant` /
`excessive_double_consonant`）は論文では Removal / Repetition に含まれ、`deletion` と
`repeated_key` からも起きる。`weak_finger_omission` は Removal の内訳として測られていない。
論文の Replacement は隣接キーに限らない（隣接が 70% の被験者が「他より多い」とされる）が、
`substitution` は隣接キーだけに置き換える。

`typo.error_types` の比率は**そのまま実績にはならない**。促音の過不足は「っ」を含む
読みにしか当てられないので、素直に引くと実績が 1/7 くらいまで落ちる。
`typo.match_error_ratios: true`（既定）は、当てられる type のうち**目標に対して
不足している type を優先して引く**ことで実績を比率に寄せる。

それでも「っ」を含む読みの数が上限になるので、促音系は要求どおりには届かない。
**`data/REPORT.md` の `error_types_achieved_ratio` に要求と実績が並ぶ**ので、そこで確認する。

#### clean サンプル

normalizer が何でも書き換えようとするのを防ぐため、`正常入力 → 同一正常入力` を必ず入れる。

1 つの読みから作れる clean は 1 件だけ（同じ `(input, target)` なので重複除去で落ちる）
なので、**達成できる clean 比率の上限は `1 / typo.variants_per_clean_sample`**。
既定は `variants_per_clean_sample: 4` と `clean_ratio: 0.25` で
「clean 1 件 + typo 3 件」。届かない設定にすると REPORT の `clean_ratio` と
`clean_ratio_requested` がずれるので気づける。

### 実 typo（JWTD）からの学習データとベンチマーク（`build-jwtd`）

合成 typo だけでは「実際の打ち間違いでどれだけ直せるか」が分からないので、
[日本語 Wikipedia 入力誤りデータセット v2.0](https://nlp.ist.i.kyoto-u.ac.jp/?日本語Wikipedia入力誤りデータセット)
（JWTD、CC BY-SA 3.0 → [LICENSES.md](LICENSES.md) の D 節）から作る。実装は
`iroha/wild/jwtd.py`。`build-all` には入らない。

```sh
./.venv/bin/python -m iroha build-jwtd --set jwtd.src=<jwtd_v2.0 を展開したディレクトリ>
```

JWTD は**表層（漢字仮名交じり）**の「直す前 / 直した後」の組なので、次の順に読みの組へ直す。

1. 差分が 1 か所で、両側とも仮名だけのものに絞る。漢字の同音誤り（固体 → 個体。train の約 4 割）は
   読みにすると同じなので対象外
2. 直した後の文を文節に切り、差分を含む文節の前後だけを窓として切り出す（読み 48 字以内。
   JWTD の文は中央値 54 字ある）
3. 両側の窓を読みにし、**読みの差が表層の仮名差分と完全に一致する**ものだけを採る
   （誤入力側は形態素解析が崩れやすい）。仮名で書かれた語は表層のまま読みにする
   （あるいは → Sudachi は あるいわ と読むが、打つのは あるいは）。
   英字・数字・読みを持つ記号（`(` → きごう）を含む窓は捨てる
4. 読みの差をローマ字の打鍵列で比べて**層**に分ける

| tier | error_type | 内容 |
|---|---|---|
| `keystroke` | `key_missing` / `key_extra` / `key_adjacent` / `key_transposition` | 打鍵列で 1 打鍵の差（「ん」は nn / n の近い方） |
| `keystroke` | `mora_duplication` | 仮名 1 つの二重打ち（をを・がが） |
| `editing` | `key_far` | 1 打鍵だが離れたキー（を → の） |
| `editing` | `mora_missing` / `mora_extra` / `mora_substitution` | 仮名単位の出し入れ。大半は助詞（郡属する → 郡に属する） |
| `editing` | `word_duplication` | 語の二重（からから）。正しい畳語（いろいろ）と区別できない |

出力（`data/jwtd/`）:

- `train.jsonl` … **keystroke だけ**＋同じ窓の clean（`jwtd.clean_ratio`）。typo 形式に
  `tier` / `category` / `page` / `pre_rev` / `post_rev` を足したもの。editing を学習に入れると、
  正しい読みに助詞を足す過剰訂正を覚える
- `bench/keystroke.jsonl`・`bench/editing.jsonl` … JWTD の test と gold から。
  typo 行（`noisy` / `clean` / `error_type`）と、同じ窓の直した後の読みを clean 行
  （`error_type: "none"`）として対にしてある。`subset`（test / gold）・`at_end`（差分が窓の末尾）・
  表層（`surface_noisy` / `surface_clean`）付き。**test・gold に出るページは train から丸ごと除く**
- `data/_stats.jwtd.json` … 段ごとに捨てた件数と理由、層・型の内訳

実測（2026-09-23）: 仮名だけの差分のうち train 225,558 組が読みの組になり、keystroke は約 6 万組
（残りは editing）。ベンチは keystroke 603 件・editing 1,621 件（＋同数の clean）。
gold（人手確認済み）の keystroke は 100 件しか無いので、数字は test と合わせて読む。

### 実誤りの分布の推定（`estimate-typo-dist`）

JWTD を**学習データではなく分布の推定**に使い、合成 typo の抽出確率に反映する。実装は
`iroha/wild/jwtd_dist.py`。入力は `build-jwtd` が毎回書く `data/jwtd/pairs_train.jsonl`
（train の全ペア＝打鍵系＋推敲系。ベンチのページは除外済み）。推定用のペアだけを作り直すなら
`--set jwtd.pairs_only=true`（`train.jsonl`・ベンチには触れない）。

```sh
./.venv/bin/python -m iroha build-jwtd --set jwtd.src=<jwtd_v2.0> --set jwtd.pairs_only=true
./.venv/bin/python -m iroha estimate-typo-dist
# → data/typo-dist/jwtd.yaml（typo: の上書き設定）、data/typo-dist/REPORT.md（比率表・除外件数・混同表）
```

出力の YAML を `load_config(path)` で重ねると、`TypoGenerator` がその比率と表で typo を作る:

- 型の比率（`error_types`）。JWTD の型を生成器の型に対応させる（key_adjacent → substitution、
  key_far → key_far、mora_missing → mora_missing …）。推敲系・二重打ち用に
  `key_far` / `mora_missing` / `mora_extra` / `mora_substitution` / `mora_duplication` / `word_duplication`
  の型がある（`default.yaml` では重み 0）
- `typo.dist`: キー別の起こりやすさ（誤りの数 / 意図した打鍵列でのキーの出現数。平均へ向けて平滑化）、
  混同表（隣接・離れたキー）、仮名の出し入れ・置換の表（直前の仮名で条件づけたものを含む）
- `second_error_ratio`（差分が複数ある組の割合）、`repeat_sokuon_bias`、`insertion_far_ratio`

**推定から除くもの:** い抜き・い足し（していた ⇄ してた。文体の書き直しで打ち間違いではない）、
読みの末尾での仮名の脱落（入力中の読みと区別できない。生成器も最後の仮名は落とさない）。
漢字の変換誤り（JWTD の約 4 割）は読み→読みでは作れないので対象外。
**推定できないもの:** clean の割合と `mixed_input`（英字の残り。JWTD では観測できない）。
`mixed_input` はローマ字の打ち間違いではないので既定では 0（`typo_dist.mixed_input` で事前の値を与えられる）。

制約: 仮名の出し入れは形態素の境界を見ないので、助詞の脱落のつもりで語の途中の仮名が落ちることがある
（抜ける仮名の割合は JWTD に合うが、位置は近似）。

### typo normalizer 用コーパス（`config/typo-corpus.yaml`）

typo normalizer の学習データを、ライセンスが明確で使いやすい 5 つのソースだけで作る設定
（2026-09-23）。Tatoeba・自前の KAKEN は使わない。

| ソース | 中身 | ライセンス | 扱い |
|---|---|---|---|
| `zenz_wiki` | zenz-v2.5-dataset / `train_wikipedia.jsonl` | CC BY-SA 4.0 | `output` の表層だけ使い、読みは Sudachi で付け直す。連続 1,000 行を 1 文書にして split を決める。JWTD のベンチと 12 字以上重なる行は捨てる |
| `llmjp_kaken` | LLM-jp Corpus v4 / `ja_kaken` | CC BY 4.0 | そのまま |
| `llmjp_egov` | LLM-jp Corpus v4 / `ja_e-gov` | CC BY 4.0 | ひらがなを含まない段落（カタカナ文語の旧法令）を捨てる |
| `llmjp_patent` | LLM-jp Corpus v4 / `ja_patent` | CC BY 4.0 | 621 ファイルから等間隔に 8 本。【】の見出し・図の参照行・符号・列挙記号を落とす |
| `llmjp_aozora` | LLM-jp Corpus v4 / `ja_aozorabunko` | CC BY 4.0 | 新字新仮名・著作権消滅の作品だけ |

**`zenz_wiki` が CC BY-SA 4.0 なので、生成物とそれで学習したモデルは CC BY-SA 4.0 になる**
（LICENSES.md の E・F 節）。

**作り方はスクリプト 1 本**（取得 → 前処理 → 読み一覧 → 2026-09-23 に作ったものとの SHA-256 照合 → 配置）:

```sh
cd dataset
./scripts/build-typo-corpus.sh balanced   # 割合を揃えたもの → data/typo-corpus/readings-balanced/
./scripts/build-typo-corpus.sh full       # 間引かないもの（document_ratio をすべて 1.0）→ data/typo-corpus/readings-full/
./scripts/build-typo-corpus.sh both
```

- 途中のファイル（元データ約 7GB・canonical）は Dropbox の外の `$IROHA_TYPO_WORK`（既定 `~/iroha-typo-data`）。`both` で約 50GB の空きが要る。終わったら消してよい
- 2026-09-24 に `both` で作り直し、balanced・full の 6 ファイルとも 2026-09-23 のものと SHA-256 が一致した（この Mac で取得を除いて約 66 分）
- 置き場所に既に一覧があれば上書きせず、照合の結果だけを出す。一致しなければ置かずに止まる
- 同じものができるように、元データの版を固定してある（`sources/zenz_wiki.py` と `sources/llmjp.py` の `REVISION`）。
  除外する評価セットは `../training/typo-normalizer/data/`（`heldout/` に JWTD のベンチと iroha-dataset の typo の
  validation / test の写し）にあり、スクリプトは最初にそろっているかを確かめる（**無いファイルは黙って飛ばされる**ため）。
  読みは Sudachi の辞書の版で変わりうる（2026-09-23 は sudachipy 0.6.11 / sudachidict-full 20260723）

一覧は別の PC で学習するため `dataset/data/typo-corpus/readings-balanced/` と
`readings-full/` に置く（Git には入らず Dropbox で同期される。説明は同じ場所の README.md）。
**2 つを混ぜて評価しない**（重複除去の都合で、片方の held-out の読みの 1 割がもう片方の train に入る）。

**typo は学習中にオンザフライで付ける**ので、出力は typo を付けない正しい読みの一覧
（`build-readings`）。`readings/{train,validation,test}.jsonl`
に 1 行 1 読み（`{"reading", "unit": "sentence" | "chunk", "source", "document_id"}`）。
打鍵列に戻せない読みを除き、読みで全体の重複を除く（同じ読みが train と test にまたがらない）。
typo 付きの example が要るときは従来どおり `build-typo`（`variants_per_clean_sample: 1`・clean 17.6%）。

- **量の釣り合い:** 全量だと Wikipedia と特許に偏るので、`document_ratio` で文書単位に間引く
  （split とは別の塩でハッシュするので、間引いても split の比率は変わらない）
- **既存の評価セットと同じ読みは作らない**（`typo.exclude_readings_from`。JWTD のベンチ、iroha-dataset の
  validation / test、training/typo-normalizer の iroha-ds・kkctx・master の valid / test）。
  よく出る文節（「〜について」など）も評価セットにあれば落ちる点に注意
- typo の型の比率は `default.yaml` のまま（Komatsu & Nakatoh 2018 に合わせたもの）

実測（2026-09-23、この Mac で preprocess 約 30 分・build-readings 約 10 分）:

| ソース | 文（canonical） | 読み（文 + 文節） | 読みの割合 |
|---|---:|---:|---:|
| `zenz_wiki`（文書の 30%） | 2,834,779 | 5,833,111 | 28.6% |
| `llmjp_kaken`（45%） | 2,209,781 | 6,104,970 | 30.0% |
| `llmjp_aozora`（60%） | 1,584,750 | 4,840,213 | 23.7% |
| `llmjp_patent`（8 本の 41%） | 1,294,961 | 2,996,532 | 14.7% |
| `llmjp_egov`（全部） | 323,745 | 609,840 | 3.0% |
| 計 | 8,248,016 | **20,384,666**（train 19,938,621 / validation 238,767 / test 207,278） | |

**ソースの割合（`document_ratio`）は実測で決めたものではない。** 全量のままだと Wikipedia と特許に
寄るのを避けるために置いた目安で、どの割合が良いかは測っていない。

### split と重複除去

**split は `document_id` 単位**（`iroha/split.py`）。乱数ではなく
`blake2b(seed:document_id)` で決めるので、データを足しても既存 document の行き先は変わらない。
同じ原文・同じ document から派生した example が train と test に混ざらないことは
`test_no_document_leaks_across_splits` で検査している。

重複除去は canonical の段で 3 段（`dedup`）:

- `exact` — 文字列そのまま
- `normalized` — 記号・空白を落として比較
- `near_duplicate` — 文字 5-gram の MinHash + LSH。**既定 OFF**（大きいデータで重いため）。
  総当たりの類似度計算はせずバンドの辞書引きだけなので、有効にしても線形時間

kkc / typo の段では `(context, input, target)` / `(input, target)` の重複も落とす。

## 設定

`config/default.yaml` が全設定。主なもの:

```yaml
seed: 42

sources:
  tatoeba: { enabled: true }
  kaken:   { enabled: true, mode: ids, min_award_year: 2016, max_projects: 200 }
  aozora:  { enabled: false }   # 将来用。3 作品での動作は確認済み・大規模では未検証

reading:
  dictionary: full
  mode: C
  reject_oov: true

context:
  max_context_chars: 256
  max_previous_sentences: 2

chunk:
  min_chars: 2
  max_chars: 40

typo:
  variants_per_clean_sample: 4
  max_errors_per_sample: 2
  clean_ratio: 0.25

split:
  train: 0.98
  validation: 0.01
  test: 0.01
```

## ソースを足す

1. `iroha/sources/base.py` の `SourceAdapter` を継承し、`info` / `download` /
   `documents` を書く（手順はその docstring）
2. モジュール末尾で `register(MyAdapter)`
3. `iroha/sources/__init__.py` の import に足す
4. `config/default.yaml` の `sources:` に既定値を書く
5. **`LICENSES.md` に節を足す**（`tests/test_config_cli.py` が検査する）

`Document.paragraphs` は「連続した文章のかたまり」のリスト。左文脈は段落の中だけで繋ぐ
（別の段落の文は前文にしない）。1 文ずつのソースは 1 段落 1 文の Document を返す。

## テスト

```sh
./.venv/bin/python -m pytest tests/ -q
```

Sudachi が入っていない環境では読み・チャンクのテストは skip される
（打鍵列・キーボード・split・重複除去・フィルタのテストは依存なしで走る）。

## 目視確認

数字だけ見ても読みの良し悪しは分からないので、**必ず `data/samples/` を見る**。
seed 固定なので再現する。

```sh
less data/samples/canonical_samples.txt   # 原文・読み・チャンク・low の理由
less data/samples/kkc_samples.txt         # context / input / target
less data/samples/typo_samples.txt        # input / target / どう崩したか
```

## 目標規模と現状

最初の目標は clean な原文 50万〜100万文、KKC example 数百万件、
typo example 500万〜1000万件。**件数を満たすために品質を落とさない。**

### 実測（2026-09-22、Tatoeba 全量 + KAKEN 90,302 課題）

| | 件数 | 目標 | |
|---|---|---|---|
| 原文（canonical） | **513,941 文**（Tatoeba 231,765 / KAKEN 282,176） | 50万〜100万文 | **達成** |
| KKC example | **3,333,277**（train 3,266,774 / valid 32,541 / test 33,962） | 数百万件 | **達成** |
| typo example | **8,985,114**（train 8,804,117 / valid 88,840 / test 92,157） | 500万〜1000万件 | **達成** |

KKC は平均 input 7.9 字 / 平均 context 65.8 字、**文をまたぐ左文脈のある example が 1,531,508 件**。
typo は平均 input 11.9 字 / clean 比率 0.176、異なる clean 読み **1,581,698 件**。
`data/` は 6.0GB・68 ファイル（うち `*.morphemes.jsonl` が約 2GB。
要らなければ `reading.record_morphemes: false`）。

#### 歩留まり

走査した 1,003,049 文のうち **513,941 文（51%）**が残った。

| ソース | document | → 文 | 1 document あたり |
|---|---|---|---|
| Tatoeba | 248,909 文 | 231,765 文 | 0.93（1 文 = 1 document） |
| KAKEN | 90,302 課題 | 282,176 文 | **3.13 文** |

捨てた内訳で大きいもの:

| 理由 | 件数 | 走査比 |
|---|---|---|
| `latin_run`（ラテン文字を含む） | 228,025 | 22.7% |
| `digits`（数字を含む） | 94,063 | 9.4% |
| `too_long` | 46,537 | 4.6% |
| 読み生成の失敗（`non_kana_reading` / `oov`） | 100,219 | 10.0% |
| 重複除去 | 11,369 | 1.1% |

**ラテン文字と数字で 32 万文（走査の 32%）を捨てている。** 学術文には
「COVID-19」「AI」「3次元」が多いので、KAKEN ではここが効く。
`filter.max_latin_run` / `filter.allow_digits` を緩めれば原文は増えるが、
**数の読み（`3日` = みっか）やラテン語彙の読みは当たらない**ので、緩めるなら
`data/samples/canonical_samples.txt` で読みを確かめてからにすること。

`reading_confidence: low` は 159,227 文（30%）。大半は固有名詞。

### 学習に足りるか（リポジトリの実測との比較）

**typo normalizer 用は十分**。`training/typo-normalizer/README.md` の実運用モデル
（Small 3.2M、clean 読み 900k × 8ep、EM 80.99%）と同じ尺度で比べると:

| | clean 読み | トークン/パラメータ（1ep 換算） |
|---|---|---|
| 実運用モデル（900k × 8ep） | 900,000 | 約 47 |
| **このデータ** | **1,581,698** | **66** |

同 README は「この先さらに計算量を増やすなら、**900k では足りなくなる**」と書いており、
その不足分を埋める量になっている。

**KKC のフルスクラッチ学習には足りない**。`training/train-full.txt` は
188,643,956 行（約 9.2B 文字、150M パラメータあたり約 61 トークン/パラメータ）。
このデータは 3.33M example / 約 250M 文字で、**150M パラメータあたり約 1.7 トークン**。
`training/README.md` は from-scratch に「全件規模のデータ推奨」としており、
typo 実験では 11.8 トークン/パラメータが「明確に学習不足」と実測されている。

**ファインチューニング・ドメイン適応には使える規模**（`training/README.md` の
健全性チェックは「100万件×2エポックで完全一致 70%+」なので、その 3 倍以上）。

### 現在のソースの上限

- **Tatoeba は打ち止め**（全 24.8 万文）
- **KAKEN は 2016年度以降に限っている**ので約 33 万課題が上限。3.1 文/課題で約 100 万文

合計 **約 120 万文 → KKC で約 800 万 example** が現在のソースの天井で、
`train-full.txt` の 188M には桁で届かない。そこを狙うならソースを足す必要がある
（`SourceAdapter` を実装する。手順は「ソースを足す」節）。
zenz-v2.5-dataset は Wikipedia 由来なので日本語 Wikipedia が最短だが、
**CC BY-SA は派生データが SA に縛られる**（`LICENSES.md` の「将来の追加候補」）。

## 動作確認済みの環境

macOS 27 / Python 3.12.3 / SudachiPy 0.6.11 / SudachiDict-full 20260723。

```sh
./scripts/verify-smoke.sh     # テスト + 小規模ビルド + サンプルの先頭を表示
```
