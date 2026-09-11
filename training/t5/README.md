# iroha 自作変換モデル（文字単位・エンコーダ／デコーダ）

llm-jp-3-150m のファインチューン（`../train.py`）が zenz に届かなかった原因分析を受けて、
**推論 30ms 以内**を制約に iroha 用に設計し直したモデルの学習パイプライン。

## 設計判断（2026-09-11）

llm-jp 系が負けた主因は「ネットワーク」ではなく**トークナイザと語彙配分**だった:

- SentencePiece がカタカナ読みを形態素と無関係に割る（ジ/シン/ガナ/カッ/タ/タメ ↔ 自信/がなかった/ため）。
  同じ読みが文脈次第で別のピース列になり、学習例が分散する
- 語彙 99,584 の埋め込み＋出力層が 102M で、150M のうち変換を考える本体は 50M（zenz-small は本体 86M）
- 英語語彙の混入は主因ではない（読みにラテン文字が無ければ英字を禁じる実験で +0.5pt のみ）

M1 Max 上でランダム重みの GPT-2 形状を `llama-bench` で較正した結果（f16、入力64トークン、生成32）:

| 形状 | params | 64トークン一括 | 生成 1トークン |
|---|---|---|---|
| 2層×768 | 24M | 1.5ms | 0.78ms |
| 3層×768 | 31M | 3.0ms | 1.20ms |
| 6層×768 | 53M | 4.2ms | 1.47ms |
| 12層×768（=zenz-small） | 95M | 6.9ms | 2.50ms |
| 16層×768 | 123M | 10.4ms | 2.64ms |
| 6層×512（=zenz-xsmall） | 26M | 3.0ms | 1.54ms |

**生成 1 ステップの時間は層数で決まり、幅（512↔768）はほとんど効かない。**
自己回帰で 30 文字出すなら 12 層は 75ms、2 層なら 24ms。一方、深いモデルでも入力の一括処理は 7ms。
→ **深いエンコーダ（読み＋左文脈を 1 回だけ双方向に読む）＋ 2 層デコーダ**が最も精度に容量を回せる。

採用: **T5 アーキテクチャ・文字単位語彙 6,572・エンコーダ 12 層×768・デコーダ 2 層×768（gated-gelu, ff 2048）≈ 114M**
（llama.cpp が `t5` としてそのまま推論でき、`ZenzEngine` は `llama_encode` → デコーダ生成で動かす。
Windows 移植でも llama.cpp の範囲に収まる）

## 3モデルのネットワーク比較

| | zenz-v3.1-small | llm-jp-3-150m（`../train.py`） | iroha T5（本ディレクトリ） |
|---|---|---|---|
| 構造 | デコーダのみ（GPT-2） | デコーダのみ（Llama） | エンコーダ・デコーダ（T5 v1.1 流） |
| 層数 × 幅 | 12 × 768 | 12 × 512 | エンコーダ 12 × 768 ＋ デコーダ 2 × 768 |
| FFN | 3072（GELU） | 2048（SwiGLU） | 2048（gated-GELU） |
| ヘッド数 | 12 | 8 | 12（d_kv 64） |
| 位置情報 | 学習済み絶対位置（1024） | RoPE | 相対位置バイアス（T5 式、バケット 32） |
| 正規化 | LayerNorm（バイアス付き） | RMSNorm | RMSNorm（T5 式） |
| トークン単位 | **文字**（BPE だが実質 1 文字） | サブワード（SentencePiece Unigram） | **文字**（1 文字 = 1 ピース） |
| 語彙数 | 6,000 | 99,584（うち日本語を含むのは 52.6%） | 6,572（ほぼ全て日本語で使う文字） |
| 総パラメータ | 95.1M | 152.3M | 約 114M |
| うち埋め込み＋出力層 | 9.2M | 102.0M | 約 10M |
| うち本体（Transformer） | 85.8M | 50.3M | 約 104M（エンコーダ 85M ＋ デコーダ 19M） |
| 事前学習 | gpt2-small-japanese-char（Wikipedia＋CC-100、文字単位） | llm-jp-3（2.1T トークン、サブワード） | なし（変換データのみでフルスクラッチ） |
| 読み→出力の対応 | 文字対文字、単調 | ピース対ピース、形態素と噛み合わない | 文字対文字、単調。エンコーダは読み全体を双方向に見る |
| 生成 1 ステップの深さ | 12 層 | 12 層 | **2 層**（＋エンコーダ出力へのクロスアテンション） |
| 実測レイテンシ（AJIMEE 平均, M1 Max） | 49.5ms | 43.6ms（f16） | 較正上の見込み 約 40ms（デコード 30 × 約 1.1ms ＋ エンコード 7ms） |
| AJIMEE acc@1 | 84.5% | 60.5〜66.0%（学習途中） | 未学習 |

読み方:

- **zenz と llm-jp の差**は、トークン単位と語彙配分にある。llm-jp は「150M」でも変換を考える本体は
  zenz の 6 割で、しかも読みが形態素と無関係に割れる。Llama 系の RoPE / SwiGLU / RMSNorm 自体は
  この規模では不利要因ではない
- **iroha T5 と zenz の差**は、同じ文字単位のまま「毎ステップ 12 層を通す」自己回帰をやめ、
  12 層はエンコーダとして 1 回だけ使い、生成は 2 層で行うこと。本体パラメータは zenz-small より多い
  （104M 対 86M）のに、30 文字生成の見込み時間は zenz-small の 8 割程度になる。
  代償は事前学習の不在で、文脈理解が足りなければ span corruption の事前学習を挟む（「未決・今後」参照）
- llm-jp の数値は `iroha-llmjp-150m-full/checkpoint-478000`（2026-09-11 時点、epoch 0.65）の実測。
  zenz の数値は Q5_K_M 量子化での実測。T5 の数値は GGUF 較正と設計値で、学習後に実測へ差し替える

## ファイル

- `build_tokenizer.py` — 学習データの文字頻度から 1文字=1ピースの SentencePiece（Unigram 形式）を組む。
  正規化 identity（全角を保つ）、ID 0=`<pad>` 1=`</s>` 2=`<unk>` 3〜5=タグ U+EE00〜02
- `tokenizer.model` — 上記の出力（train-10m.txt 先頭 200 万行、出現 3 回以上、6,572 語彙）
- `train_t5.py` — HF `T5ForConditionalGeneration` をランダム初期化から学習。データは `../prepare_data.py` の
  出力形式（`[U+EE02文脈]U+EE00読みU+EE01出力`）をそのまま読み、U+EE01 までを（`</s>` 付きで）エンコーダ入力、
  後ろをデコーダ目標にする。`--eval-samples` で末尾を検証用に切り出して eval loss を記録する
- GGUF 変換は既存の `../convert-gguf.sh <出力ディレクトリ>`（T5 経路が `tokenizer.model` を直接読む）

llama.cpp 側のトークン化が学習側（sentencepiece）と一致することは 3,000 行で確認済み（不一致 0）。
文字単位なので構造的に一致する（llm-jp で問題になった Viterbi と近似の食い違いが起きない）。

## 手順

```bash
cd training/t5
# 0. 依存: ../../.venv に torch transformers datasets sentencepiece protobuf
# 1. トークナイザ（既に生成済み。データを変えたら作り直す）
python3 build_tokenizer.py --data ../train-10m.txt --lines 2000000 --min-count 3 --out ./tokenizer.model

# 2. 疎通確認（M1 Max, 3万件, 7M params, 約2分）
python3 train_t5.py --data <3万行> --out /tmp/iroha-t5-smoke --enc-layers 4 --dec-layers 2 --d-model 256 \
    --d-ff 768 --heads 4 --batch-size 32 --grad-accum 1 --lr 1e-3 --warmup-steps 50 --eval-samples 1000

# 3. パイロット（GPU。1,000万件・本番形状。llm-jp の 10m ランと同じデータ量で比較する）
python3 train_t5.py --data ../train-10m.txt --out ./iroha-t5-e12d2-10m \
    --enc-layers 12 --dec-layers 2 --d-model 768 --d-ff 2048 --heads 12 \
    --epochs 1 --batch-size 64 --grad-accum 1 --lr 5e-4 --warmup-steps 2000 --eval-samples 10000
#    実効バッチ = batch-size × grad-accum × GPU数。256 前後を目安に調整

# 4. 本番（1.89 億件、1 エポック）
python3 train_t5.py --data ../train-full.txt --out ./iroha-t5-e12d2-full （形状は 3 と同じ）

# 5. GGUF 化と評価（チェックポイント途中でも可: tokenizer.model と config.json を添える）
../convert-gguf.sh ./iroha-t5-e12d2-10m        # → ~/Library/Application Support/iroha/models/iroha-t5-e12d2-10m-f16.gguf
../../macos/scripts/bench-compare.sh ~/Library/Application\ Support/iroha/models/zenz-v3.1-small-Q5_K_M.gguf \
    ~/Library/Application\ Support/iroha/models/iroha-t5-e12d2-10m-f16.gguf
```

## 判断の目安

- パイロット（1,000 万件）で AJIMEE acc@1 が zenz-xsmall（68.5%）を超えれば本番へ。
  llm-jp の 10m ランや full ランの途中値（60〜66%）を超えられないなら、デコーダ層数（1/3 層）と
  エンコーダ幅の前に、データ量ではなく学習率（5e-4 → 1e-3）と warmup を疑う
- eval loss と AJIMEE を並べて見る（llm-jp の full ランでは損失が下がり続けているのに
  AJIMEE が横ばい／悪化した。200 件のノイズ幅は ±5pt）
- レイテンシは `bench` の平均で見る。較正上は AJIMEE 平均（出力約 30 文字）で
  エンコード 7ms ＋ 30×約1.1ms（デコーダ 2 層＋クロスアテンション）≒ 40ms、
  eval.tsv やライブ変換の文節長では 30ms を切る見込み。超えるならデコーダを 1 層にする

## 未決・今後

- 事前学習（span corruption）を挟むか: 1.89 億件の変換対の出力側自体が数 B 文字の日本語コーパスなので、
  まず変換データのみで学習し、文脈理解の不足が見えたら検討
- 出力側だけ語ピース（小さな BPE）にしてステップ数を減らす案は、精度が足りて速度が足りないときの選択肢
- 予測変換（`PredictionEngine`）はデコーダ専用モデル前提のため、エンコーダ・デコーダ型では無効（空を返す）
