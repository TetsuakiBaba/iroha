# 候補選択専用の小型 Decision Model（Reranker）の検証（2026-09-18）

## 問い

かな漢字変換の候補選択では、汎用 Language Model の生成能力やモデルサイズよりも、
**reading / context / candidate の関係を直接学習した小型の判別モデル**の方が効率的に候補順位を
学べるか。モデルは文字列を生成せず、`context + reading + candidate → score`（スカラー）だけを出す。
生成 LM のトークン尤度で並べる方式（zenz 再採点・Qwen 系列尤度、`experiments/jev/`）とは
コードも数値も分けて記録する。

前提として、判定器がどれだけ良くても候補プールの oracle は超えない
（AJIMEE 200 件: ラティスのみ 138、ラティス＋zenz生成 177。`experiments/jev/README.md`）。
この実験が測るのは「**同じプール上で**、小さな判別モデルが生成 LM の再採点に勝てるか」で、
変換全体の精度ではない。

## 構成

- **入力列**（比較実験の 3 条件は `--fields` で切り替える）
  - `cand`: `[CLS] U+EE01 cand </s>`
  - `reading+cand`: `[CLS] U+EE00 reading U+EE01 cand </s>`
  - `ctx+reading+cand`: `[CLS] U+EE02 ctx U+EE00 reading U+EE01 cand </s>`
  - 位置埋め込み（学習、最大 320）とセグメント埋め込み（特殊 / 文脈 / 読み / 候補）を足す
- **モデル** `model.py`: `nn.TransformerEncoder`（pre-LN, GELU）→ `[CLS]` の隠れ状態 → `Linear(d, 1)`。
  自己回帰生成はしない。設定（起動時にパラメータ数を表示。埋め込み / 非埋め込みに分ける）:

  | 設定 | d_model | 層 | ヘッド | d_ff | 合計 | 埋め込み | 非埋め込み |
  |---|---|---|---|---|---|---|---|
  | xs | 256 | 4 | 4 | 1024 | 4.93M | 1.77M | 3.16M |
  | s | 512 | 6 | 8 | 2048 | 22.45M | 3.53M | 18.92M |
  | m | 768 | 6 | 12 | 3072 | 47.83M | 5.30M | 42.53M |
  | l | 768 | 12 | 12 | 3072 | 90.35M | 5.30M | 85.06M |
  | xl | 1024 | 12 | 16 | 4096 | 158.22M | 7.06M | 151.16M |

- **トークナイザ** `tokenizer.py`: `training/t5/tokenizer.model` の**文字単位**語彙（6,572 + `[CLS]`）を
  そのまま使う。採用理由: かな漢字変換は読みと候補が文字単位で単調に対応するので、サブワード分割の
  揺れ（llm-jp で観測した学習時と推論時の分割不一致、`training/README.md`「トークン化の一致」）が
  入らない。語彙が小さく埋め込み表が 3.5M（`s`）で済み、パラメータを Transformer 本体に回せる。
  既存の T5 と同じ語彙なので比較もしやすい。全ピースが 1 文字なので dict で写し、起動時に
  sentencepiece の `encode` と一致することを確かめる（`self_check`）
- **学習データ** `data.py`: `training/train-1m.txt`（zenz-v2.5-dataset。読みはカタカナ、左文脈は
  末尾 40 文字、文脈付きの行は約 16%）の各行に、`iroha-cli lattice-dump` で辞書ラティス
  （azooKey）の読み一致候補を付ける。1 グループ = 1 読み: 正例 = 正解、負例 = 正解と一致しない
  ラティス候補の上位 K−1（K=8、ラティス順）。**負例は同じ読みに対する実際の誤変換候補**
  （「橋を渡る / 箸を渡る / 端を渡る」型の hard negative）で、無関係な文は使わない。
  無変換の仮名候補（カタカナ・ひらがなのまま）は学習では残す（正解がカタカナのままの行が 5%、
  ひらがな等が 9% あり、落とすと偽を教える）。dev は読みのハッシュで 2% を分ける
- **損失** `train.py`: 既定は **pairwise（RankNet）**: グループ内の全 (正例, 負例) 対で
  `−log σ(s_pos − s_neg)` を対で平均し、グループで平均する。`--loss margin`（MarginRankingLoss）、
  `--loss listwise`（正例 1 つなので softmax CE）を比較用に持つ。
  AdamW lr 3e-4, warmup 500, 線形減衰, clip 1.0, dropout 0.1, fp32（MPS の bf16 autocast は無効）
- **評価** `eval_ajimee.py`: `experiments/jev/ajimee-candidates.jsonl`（AJIMEE 200 件の候補プール）を
  そのまま使い、同じプール上で Top-1 / MRR / oracle を、ラティス順位・zenz 再採点・llm-jp 150M
  再採点・Qwen 系列尤度と並べる。実行のたびに参照値（oracle 138 / 177、zenz 再採点 169）を再現して
  から表を出す。レイテンシは 1 件（候補集合 1 バッチ）の中央値・平均で、ラティス生成（約 9〜13ms）
  は含まない

## 手順

```sh
# 1. ラティス候補付きの学習データ（zenz は読まない。1 行 9ms 前後。--skip/--limit で並列化できる）
cd macos && swift build -c release --product iroha-cli
.build/release/iroha-cli lattice-dump ../training/train-1m.txt \
    ../experiments/reranker/data/train-1m-100k-lattice.jsonl --limit 100000 --n 10
# 2. llm-jp 150M の参照値（同じプールを llm-jp の尤度で採点）
IROHA_MODEL=../training/iroha-llmjp-150m-full-f16.gguf .build/release/iroha-cli ajimee-dump \
    ../testdata/ajimee/evaluation_items.json ../experiments/reranker/data/ajimee-candidates-llmjp150m.jsonl
cd ..
# 3. データの統計と入力列の長さ
.venv/bin/python experiments/reranker/data.py experiments/reranker/data/train-1m-100k-lattice.jsonl
# 4. 学習（3 条件）
cd experiments/reranker
../../.venv/bin/python train.py --dump data/train-1m-100k-lattice.jsonl --config s --fields ctx+reading+cand --epochs 1 --out runs/s-ctx
../../.venv/bin/python train.py --dump data/train-1m-100k-lattice.jsonl --config s --fields reading+cand     --epochs 1 --out runs/s-read
../../.venv/bin/python train.py --dump data/train-1m-100k-lattice.jsonl --config s --fields cand             --epochs 1 --out runs/s-cand
# 5. 評価（参照値の再現 → 比較表）
../../.venv/bin/python eval_ajimee.py runs/s-ctx runs/s-read runs/s-cand --train-dump data/train-1m-100k-lattice.jsonl --out results/s-100k.json
```

スケール（20M → 50M → 100M → 150M）は `--config m / l / xl`、データは `lattice-dump` の `--limit` と
`train-10m.txt` で増やす。スクリプトは変えない。

## 学習データの統計（train-1m.txt 先頭 100k 行、2026-09-18）

`lattice-dump` は 1 行 8.8ms（4 並列で 100k 行を約 4 分）。

| 項目 | 値 |
|---|---|
| 行数 | 100,000（形式不正 30 行を除く） |
| 学習に使えるグループ（負例が 1 つ以上ある読み） | 99,595（学習 97,681 / dev 1,914） |
| 負例数の分布（K=8、上位 7 まで） | 7: 41%、6: 35%、5: 10%、4: 7%、3: 5%、2: 2%、1: 0.5% |
| 正解がラティス候補に含まれる | 64.1%（dev 62.7%） |
| ラティス 1 位が正解 | 45.6%（dev 43.2%） |
| 入力列の平均長 / 99% / 最大（文字） | cand 25 / 83 / 114、reading+cand 52 / 179 / 226、ctx+reading+cand 54 / 180 / 226 |

学習の 1 グループは正解を必ず候補に含むので、dev の Top-1 の上限は 100%。ラティスの参照値
（dev 43.2%）は「正解が候補にない 37% を 0 と数えた」値で、モデルの dev Top-1 と直接は比べられない。
比べるのは AJIMEE の同一プール上の数値。

## 結果（2026-09-18、M1 Max 64GB、MPS fp32、設定 s = 22.45M、train-1m.txt 先頭 100k 行、1 エポック、RankNet、K=8）

### AJIMEE-Bench 200 件、同一プール上の Top-1 / MRR（`results/s-100k-1ep.json`）

学習ダンプに AJIMEE の読みは含まれない（0/200）。プールは `experiments/jev/ajimee-candidates.jsonl` のまま。

| 採点方式 | ラティスのみ (oracle 138) | 同・仮名なし | ラティス+zenz生成 (oracle 177) | 同・仮名なし |
|---|---|---|---|---|
| ラティス順位 1 位（辞書のみ） | 91 (45.5%) MRR 0.555 | 91 (45.5%) 0.556 | 91 (45.5%) 0.580 | 91 (45.5%) 0.589 |
| zenz-v3.1-small 再採点（生成 LM 尤度、95M） | **132 (66.0%)** 0.674 | 132 (66.0%) 0.674 | **169 (84.5%)** 0.864 | 169 (84.5%) 0.864 |
| iroha-llmjp-150m 再採点（生成 LM 尤度） | 127 (63.5%) 0.661 | 128 (64.0%) 0.664 | — | — |
| Qwen3-4B 系列尤度（生成 LM、jev 実験） | — | — | — | 163 (81.5%) 0.847 |
| Qwen2.5-7B 系列尤度（生成 LM、jev 実験） | — | — | — | 166 (83.0%) 0.856 |
| **Decision Model** ctx+reading+cand | 41 (20.5%) 0.382 | 43 (21.5%) 0.387 | 56 (28.0%) 0.487 | 58 (29.0%) 0.492 |
| **Decision Model** reading+cand | 44 (22.0%) 0.396 | 46 (23.0%) 0.401 | 58 (29.0%) 0.500 | 60 (30.0%) 0.506 |
| **Decision Model** cand のみ | 46 (23.0%) 0.395 | 47 (23.5%) 0.398 | 57 (28.5%) 0.492 | 58 (29.0%) 0.495 |

参考: zenz-v3.1-small の自由生成（プール外）= 171/200 (85.5%)。

### dev（学習ドメイン、1,914 グループ）の Top-1 の推移（500 ステップごと、%）

| 条件 | 500 → 1000 → 1500 → 2000 → 2500 → 3000 | 学習時間 |
|---|---|---|
| s-ctx | 42.9 → 46.1 → 48.3 → 49.0 → 49.9 → 50.3 | 66.2 分 |
| s-read | 43.6 → 47.2 → 47.8 → 49.0 → 49.4 → 50.4 | 54.3 分 |
| s-cand | 44.7 → 48.0 → 48.2 → 49.2 → 50.0 → 50.7 | 20.8 分 |

- 正解がラティスに含まれる dev グループ（1,200 件）だけ見ると、ctx+reading+cand のモデル Top-1 は 50.3%、
  **ラティス自身の順位 1 位は 69.0%**。正解がラティスにない 714 件でも 50.3% で、両者に差がない
- 3 条件の dev Top-1 は 50.3 / 50.4 / 50.8% で**差がない**。読みも文脈も使われていない

### レイテンシ・サイズ（1 件 = その候補集合を 1 バッチで採点。ラティス生成 9〜13ms は含まない）

| 条件 | パラメータ 合計 / 非埋め込み | best.pt | MPS 中央値 / 平均 | CPU 中央値 / 平均 |
|---|---|---|---|---|
| ctx+reading+cand | 22.4M / 18.9M | 90MB | 9.4 / 10.6 ms | 35.1 / 39.9 ms |
| reading+cand | 22.4M / 18.9M | 90MB | 9.6 / 10.9 ms | 32.8 / 36.7 ms |
| cand | 22.4M / 18.9M | 90MB | 7.2 / 8.3 ms | 25.2 / 28.2 ms |

比較: zenz-small の再採点（同じ候補集合、llama.cpp Metal）は 20ms、生成は 42ms。

### 読み取り

1. **この規模（22M・100k 読み・1 エポック）では成立していない。** 同一プール上で zenz 再採点 132、
   llm-jp 150M 再採点 127 に対し 41〜46。しかもラティスの並び順（91）にも負ける。
   dev でも、正解がラティスにあるグループでラティス 1 位 69.0% に対しモデル 50.3%
2. **3 条件に差がないことが原因を特定している。** `cand` だけで同じ精度なので、モデルは候補文字列の
   「日本語らしさ」（頻度の高い字面）だけを学び、読み↔候補の対応も文脈も使えていない。
   AJIMEE の誤りは「依頼→以来」「不逮捕→負逮捕」「新暦→新歴」「多変数→他変数」型で、
   まさに「この読みにこの漢字が対応するか」の知識が欠けている。1 対ごとに 1 ビットしか教えない
   pairwise 損失では、文字単位の系列尤度（全文字に損失がかかる）に比べて 1 例あたりの教師信号が薄く、
   100k 読みでは足りない
3. **速度は狙いどおり速い。** 22M で 1 件 9〜10ms（MPS）。ここは仮説の通りで、精度が付いてくれば
   zenz 再採点 20ms の半分。ただし精度が出るまでモデルやデータを大きくすると、この利点は縮む
4. dev Top-1 はまだ上昇中（42.9 → 50.3%、飽和していない）なので、**データとエポックを増やした
   性能曲線を見ないと仮説の可否は決められない**。ただし今の傾きは緩く、1 エポック 100k で
   ラティス順位（69%）に届かない距離を考えると、データを 10 倍にして届くかどうかという勘定

### 次の段階（このスクリプトのまま回せる）

1M 行のラティスダンプは作ってある（`data/train-1m-lattice.jsonl`、999,987 行、604MB、Dropbox で
GPU 機に同期される）。この Mac では 1 エポック 100k で 55〜66 分かかるので、1M × 数エポックは
GPU 機（`/home/baba/venvs/imellm-training`、`train.py` は CUDA を自動選択）で回す:

```sh
cd /data1/Dropbox/project/iroha/experiments/reranker
/home/baba/venvs/imellm-training/bin/python train.py --dump data/train-1m-lattice.jsonl \
    --config s --fields reading+cand --epochs 3 --batch-groups 64 --eval-every 2000 --out runs/s-1m-3ep
# 20M → 50M → 100M → 150M の曲線: --config s / m / l / xl を同じデータで
# 文脈の効果は、文脈付きの行だけを増やしたデータでないと見えない（今の学習データは 16% しか文脈がない）
```

見るもの: dev Top-1 が**ラティス 1 位 69%（正解を含むグループ）を超えるか**、そして `cand` 条件との差が
開くか（開かなければデータを増やしても読みを学んでいない）。AJIMEE はノイズ幅 ±5pt なので dev を主に見る。

## 判断の目安

- **成立性**: ラティスのみプール（候補集合が全方式で同一）で、本モデルの Top-1 が zenz-small 再採点
  （132/200）と llm-jp 150M 再採点（127/200）を上回るか。上回れば「サイズより関係の直接学習」を支持する
- **3 条件の差**: `cand` だけで高ければ日本語らしさを見ているだけ。`reading+cand` で伸びれば読み制約を
  使っている。`ctx+…` の伸びは、学習データの文脈付き行が 16% しかないので小さいと予想する
- AJIMEE 200 件のノイズ幅は ±5pt。dev（約 2,000 グループ）の Top-1 / MRR を併記して判断する
- 本モデルの上限はプールの oracle（ラティスのみ 138、＋zenz生成 177）。変換全体の精度の議論は
  候補生成側（`training/t5/`）の話であって、ここでは扱わない
