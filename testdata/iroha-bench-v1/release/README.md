# iroha-bench-v1

日本語かな漢字変換の評価セットです。3,253 項目。各項目は、カタカナの読み（`input`）・左文脈（`context_text`）・
正解の表記（`expected_output`）からなります。かな漢字変換モデル zenz の学習データ zenz-v2.5-dataset の出典
（日本語 Wikipedia と Common Crawl）と重ならない文から作りました。

論文: 馬場 哲晃「浅いデコーダを持つ文字単位エンコーダ・デコーダモデルによる高速なニューラルかな漢字変換」（Jxiv、2026）

## ファイル

`iroha-bench-v1.jsonl`（1 行 1 項目、UTF-8）

| フィールド | 中身 |
|---|---|
| `index` | 項目の ID（`<出典>-<番号>`） |
| `source` | 出典（下の表） |
| `context_text` | 左文脈（同じ文書の直前の文字列、末尾 40 文字まで。無い項目は空） |
| `input` | 読み（カタカナ） |
| `expected_output` | 正解の表記（1 項目に 1 つ） |
| `license` | その項目のライセンス |
| `attribution` | その項目の出典の表記 |

`index`・`context_text`・`input`・`expected_output` は AJIMEE-Bench（https://github.com/azooKey/AJIMEE-Bench）と
同じ名前と意味です。AJIMEE-Bench と同じ JSON 配列が要る場合は次で変換できます。

```sh
python3 -c "import json,sys; print(json.dumps([json.loads(l) for l in open(sys.argv[1])], ensure_ascii=False))" iroha-bench-v1.jsonl > evaluation_items.json
```

## 出典と項目数

| 区分 | `source` | 出典 | 項目数 | ライセンス |
|---|---|---|---|---|
| 書き言葉 | `llmjp_kaken` | LLM-jp Corpus v4 ja_kaken（科研費の研究概要） | 411 | CC BY 4.0 |
| | `llmjp_egov` | LLM-jp Corpus v4 ja_e-gov（法令） | 340 | CC BY 4.0 |
| | `llmjp_patent` | LLM-jp Corpus v4 ja_patent（特許公報） | 432 | CC BY 4.0 |
| | `llmjp_aozora` | LLM-jp Corpus v4 ja_aozorabunko（青空文庫。著作権の切れた新字新仮名の作品） | 452 | CC BY 4.0 |
| 話し言葉 | `realpersonachat` | RealPersonaChat（nu-dialogue） | 395 | CC BY-SA 4.0 |
| | `mrmp` | Multi-Relational Multi-Party Chat Corpus（nu-dialogue） | 366 | CC BY-SA 4.0 |
| | `jmrd` | JMRD（京都大学 黒橋・褚・村脇研究室） | 344 | CC BY-SA 4.0 |
| | `newschat` | 感想付きニュース雑談対話コーパス（Fuka Narita） | 355 | MIT |
| | `jcre3` | J-CRe3（理化学研究所 ほか） | 158 | CC BY-SA 4.0 |

## 作り方

- 文から 8〜40 文字の部分を切り出し、読みを SudachiPy（辞書 SudachiDict-full）で付けました。読みは推定なので誤りを含みます
- 読みの推定が不確かな文（別の分割と読みが食い違う語、辞書に読みの無い語、読みが長すぎる語を含む文）と、
  数字・英字を含む文は除きました
- 項目の半数に左文脈を付けました（最終的に 1,109 項目）。1 つの文書から取る項目は 2 個までです
- 正解の表記が zenz-v2.5-dataset から作った学習データの出力にそのまま現れる項目（933 項目）は除きました
- 正解は 1 項目に 1 つで、表記の揺れ（「美味しい／おいしい」など）も誤答になります

作成のコードは https://github.com/TetsuakiBaba/iroha の `dataset/iroha-typo-normalizer/`（`build-iroha-bench`、
設定 `config/iroha-bench-v1.yaml`）にあります。

## 評価の手順（論文の条件）

変換は iroha のコマンドラインツール `iroha-cli`（https://github.com/TetsuakiBaba/iroha の `macos/`）で行いました。

```sh
cd macos && swift build -c release --build-system native --product iroha-cli
IROHA_MODEL=<モデルの GGUF> IROHA_LATTICE=off IROHA_TIMINGS=<モデル名>.run1.tsv \
  .build/release/iroha-cli ajimee evaluation_items.json > <モデル名>.off.txt
```

- `IROHA_LATTICE=off` は辞書を使わずモデルの貪欲生成だけで変換する条件、`IROHA_TIMINGS` は項目ごとの変換時間の出力先です
- 正解率の表は `tools/summarize.py --items evaluation_items.json --iroha "<名前>=<モデル名>.off.txt" ...`、
  変換時間の表は `tools/latency_stats.py <TSV のディレクトリ>` で作れます

## ライセンス

この評価セット全体は **CC BY-SA 4.0** で配布します。各項目は、元の出典のライセンス（`license`）と
出典の表記（`attribution`）にも従ってください。詳しくは `LICENSE.md` にあります。

- すべての項目は、元の文を文に分割し、部分を切り出し、読みを付けるなどの加工をしています
- 科研費の研究概要は KAKEN（科学研究費助成事業データベース、国立情報学研究所）に由来します。
  この評価セットは国立情報学研究所が作成したものではありません
