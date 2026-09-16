# 自作変換モデルの開発（M7〜）

irohaの変換エンジンは [ConversionEngine](../Sources/IrohaCore/ConversionEngine.swift)
プロトコルで抽象化されており、**GGUF形式のモデルファイルを差し替えるだけ**で
別のモデルを使える（設定ウィンドウ > 変換モデル、または `defaults write ... modelPath`）。

差し替えの動作実績（M1 Max。eval.tsv=独自評価セット40文、AJIMEE=zenzai公式ベンチ200件、
2026-08-30計測 `scripts/bench-compare.sh`）:

| モデル | サイズ | eval.tsv 完全一致 | eval.tsv CER | AJIMEE acc@1 | AJIMEE MinCER | 平均レイテンシ |
|---|---|---|---|---|---|---|
| zenz-v3.1-small (95M, Q5_K_M) | 70MB | 90.0% | 4.5% | 85.5% (171/200) | 1.71% | 70.4ms |
| zenz-v3.1-xsmall (Q5_K_M) | 20MB | 90.0% | 4.2% | 68.5% (137/200) | 5.01% | 21.9ms |

（AJIMEEの平均レイテンシは入力が長いためeval.tsvより大きく出る。この表と2026-09-16より前の計測は
IME本体のユーザ辞書・学習が乗った状態の値。以後は空にして測る。下記「比較の標準条件」）

## 自作モデルの学習パイプライン（基盤: llm-jp-3-150m）

基盤モデルは [llm-jp/llm-jp-3-150m](https://huggingface.co/llm-jp/llm-jp-3-150m)
（Llama系 150M、SentencePiece Unigram 語彙99,574、Apache-2.0、2.1Tトークン事前学習済み）。
GGUF変換に必要な `tokenizer.model` がHFリポジトリに含まれないため、
llm-jp-tokenizer 公式リポジトリから取得して同じディレクトリに置く（重要）。

```
0. 環境準備      python3 -m venv .venv && .venv/bin/pip install torch transformers datasets accelerate sentencepiece gguf
   基盤モデル    huggingface-cli download llm-jp/llm-jp-3-150m --local-dir ./base/llm-jp-3-150m
                 curl -sL -o ./base/llm-jp-3-150m/tokenizer.model \
                   https://github.com/llm-jp/llm-jp-tokenizer/raw/main/models/ver3.0/llm-jp-tokenizer-100k.ver3.0b1.model
1. データ準備    python3 prepare_data.py --samples 1000000 --out train-1m.txt
                 （zenz-v2.5-dataset: 読み/変換結果/左文脈の約1.29億件、HFで公開）
2. 語彙GGUF      ./make-vocab-gguf.sh   → llm-jp-3-150m-vocab.gguf（トークン化一致用）
3. 学習          pip install llama-cpp-python
                 python3 train.py --data train-1m.txt --out ./iroha-llmjp-150m --epochs 2 \
                     --llama-vocab llm-jp-3-150m-vocab.gguf
                 （U+EE00〜02を特殊トークンとして追加、U+EE01以降のみ損失計算。
                   CUDA GPU推奨。実効バッチ = --batch-size × --grad-accum = 256。
                   --llama-vocab は必須級: 下記「トークン化の一致」参照）
4. GGUF変換      ./convert-gguf.sh ./iroha-llmjp-150m
5. 評価          IROHA_MODEL=<gguf> ../.build/debug/iroha-cli bench ../testdata/eval.tsv
6. zenzとの比較  ../scripts/bench-compare.sh <zenz.gguf> <学習済み.gguf>（下記「zenzとの比較」）
7. IMEに反映     設定ウィンドウでモデルパスを指定 → irohaを再起動
```

## トークン化の一致（--llama-vocab、2026-08-31導入）

**背景**: HFトークナイザ（SentencePiece Unigramの真のViterbi）とllama.cpp
（バイグラム結合による近似）は、同じ語彙でも分割が食い違うことがある
（例: `▁キョウ` vs `▁キ`+`ョ`+`ウ`）。checkpoint-6000での実測では、
AJIMEE-Benchのプロンプト200件中118件（59%）で学習時と推論時のトークン列が
不一致で、acc@1は一致82件で53.7%、不一致118件で33.9%と約20ポイントの差が出た。
データを増やしてもこの構造的不利は解消しない。

**対策**: `make-vocab-gguf.sh` で語彙のみのGGUFを作り、`train.py --llama-vocab`
を指定すると、学習データのトークン化にllama.cpp本体（llama-cpp-python）を使う。
これで学習時と推論時（ZenzEngineの `llama_tokenize(addSpecial: true)`）の
トークン列が読み・文脈・全体で構造的に一致する。推論側の変更は不要。

**検証**: `verify_tokenization.py` で確認できる。

```bash
python3 verify_tokenization.py --llama-vocab llm-jp-3-150m-vocab.gguf \
    --data train-1m.txt --samples 2000 \
    --llama-tokenize-bin <llama-tokenizeバイナリ>   # vendor版との照合（任意）
```

注意: --llama-vocab で学習したモデルと従来学習のモデルは分割の分布が違うため、
チェックポイントを跨いだ学習再開はしないこと（新しいランとして最初から学習する）。

## zenzとの比較ベンチマーク（AJIMEE-Bench）

zenzai公式の評価方式 [AJIMEE-Bench](https://github.com/azooKey/AJIMEE-Bench)（味見ベンチ）に
準拠した比較ができる。日本語Wikipedia入力誤りデータセット(v2)由来の200件
（左文脈付き100件 + 文脈なし100件）で、指標は本家 `utils.py` と同じ:

- **acc@1**: グリーディ変換の第1候補が許容解リストのいずれかに完全一致した割合
- **MinCER**: 各許容解とのCER（編集距離/正解長）の最小値を全件平均したもの

```bash
# データ取得（CC-BY-SA 3.0のためリポジトリには含めない。testdata/ajimee/ に保存）
scripts/fetch-ajimee.sh

# 単体実行
IROHA_MODEL=<gguf> .build/release/iroha-cli ajimee testdata/ajimee/evaluation_items.json

# 複数モデルの一括比較（eval.tsv + AJIMEE-Bench、Markdown表を出力）
scripts/bench-compare.sh \
  ~/Library/Application\ Support/iroha/models/zenz-v3.1-small-Q5_K_M.gguf \
  training/iroha-llmjp-150m/iroha-llmjp-150m-f16.gguf
```

### 比較の標準条件（2026-09-16以降。学習したモデル vs zenz small / xsmall）

今後、学習させたモデルは **zenz-v3.1-small と zenz-v3.1-xsmall（いずれも Q5_K_M）** を
基準にして、AJIMEE-Bench を次の **2条件で分けて**比較する。1条件だけの数値は載せない。

| 条件 | 環境変数 | 意味 |
|---|---|---|
| NNのみ | `IROHA_LATTICE=off` | モデルの貪欲生成そのもの。ライブ変換の第一候補と同じ経路 |
| 辞書ラティス + NN再採点 | `IROHA_LATTICE=always` | azooKey辞書ラティスのn-best 10件 + モデル自身の生成1件をモデルの対数確率で並べ、1位を答えにする |

- ユーザ辞書・学習は `iroha-cli bench / ajimee` の既定で**空**になる（IME本体のデータを混ぜると
  学習ファイルの成長で数値が変わり再現できない）。混ぜて測るときだけ `IROHA_WITH_USER_DATA=1`
- 量子化は Q5_K_M で揃える（f16 と混ぜない）
- 「NNのみ」がモデルの素の力、「辞書 + NN」が辞書に助けられた実力。両者の差が
  「辞書で補えている弱点の大きさ」を表す（zenz small −1pt / xsmall +1.5pt、自作モデルは +13pt）
- `scripts/bench-compare.sh` がこの2条件を1行に並べた表を出す（eval.tsv は NNのみ）:

```bash
scripts/bench-compare.sh \
  ../training/zenz-v3.1-small-Q5_K_M.gguf ../training/zenz-v3.1-xsmall-Q5_K_M.gguf <学習済み.gguf>
# 単体で回すとき（誤答リストを見る）
IROHA_MODEL=<gguf> IROHA_LATTICE=off    .build/release/iroha-cli ajimee ../testdata/ajimee/evaluation_items.json
IROHA_MODEL=<gguf> IROHA_LATTICE=always .build/release/iroha-cli ajimee ../testdata/ajimee/evaluation_items.json
```

基準値（2026-09-16、M1 Max、`scripts/bench-compare.sh` の出力。ユーザ辞書・学習は空）:

| モデル | eval.tsv 完全一致 | eval.tsv CER | AJIMEE acc@1 (NNのみ) | MinCER | ms | AJIMEE acc@1 (辞書+NN) | MinCER | ms |
|---|---|---|---|---|---|---|---|---|
| zenz-v3.1-small-Q5_K_M | 90.0% | 4.47% | 84.5% (169/200) | 1.57% | 44.7ms | 83.5% (167/200) | 1.68% | 79.1ms |
| zenz-v3.1-xsmall-Q5_K_M | 90.0% | 4.22% | 67.5% (135/200) | 5.05% | 23.8ms | 69.0% (138/200) | 4.77% | 46.0ms |
| iroha-llmjp-150m-full-Q5_K_M | 67.5% | 10.42% | 59.0% (118/200) | 7.36% | 39.0ms | 72.0% (144/200) | 3.75% | 93.6ms |

- 文脈なし/あり（各100件）の内訳: small NNのみ 89/80、辞書+NN 87/80。xsmall NNのみ 70/65、辞書+NN 71/67。
  llmjp-full NNのみ 60/58、辞書+NN 76/68
- レイテンシは同時に動いている処理で±30ms程度ぶれる。モデル間の相対比較にだけ使う
- eval.tsv は自分の文なので学習データが乗ると上振れする（llmjp-full は学習込みの旧計測 72.5% →
  空にすると 67.5%）。2026-09-16 より前の eval.tsv の値は比較に使わない
- zenz は辞書を足してもほぼ変わらない（small は2件落ち: 名付けた/名づけた、後10年→50年。xsmall は3件直り0件悪化）。
  自作モデルは27件直り1件悪化で、生成の語彙選択の弱さ（group/解説→開設）を辞書が補っている
- 上の `always` は現在の `LatticeRescoringEngine` の実装（モデル生成候補も一緒に採点）での値。
  CLAUDE.md や同ファイルのコメントにある「ラティス再採点 66.5%」は生成候補を混ぜる前の旧計測
- zenzai 公式の AJIMEE 報告は「Zenzai (small) 80.0 / (medium) 86.5」のようにシステム単位で、
  モデル単体と辞書込みを分けていない（Zenzai は辞書をドラフトにした投機的デコーディングなので
  報告値は常に辞書込み）。この2条件分けは iroha 独自の見方

#### 「NNのみ」はまだ ReadingConstraint の影響を含む（2026-09-16 判明）

`IROHA_LATTICE=off` でも `ZenzEngine.generate` は `ReadingConstraint` で
トークンをマスクするため、純粋な貪欲生成ではない（200件中177件で制約が有効）。
学習マシン側で llama-cpp-python から直接・制約なしで同じ200件を測ると:

| モデル（Q5_K_M、文脈40字に切り詰め） | 制約なし acc@1 | iroha「NNのみ」 | 差 |
|---|---|---|---|
| zenz-v3.1-small | 84.0% | 84.5% | −0.5 |
| zenz-v3.1-xsmall | 67.5% | 67.5% | ±0 |
| iroha-llmjp-150m-full | **70.5%** | **59.0%** | **+11.5** |

zenz（文字単位・語彙6000）では両者が一致するのに、自作モデル（サブワード・語彙99,574）
だけ11.5ポイント下がる。ReadingConstraint は「文字の読み位置」をビットマスクで
追う設計なので、1トークンが複数文字にまたがるサブワード語彙と噛み合っていない可能性が高い。
確認するには制約を丸ごと切る実験フラグが要る（`IROHA_NO_LATIN` はラテン文字の
サブルールだけで、制約自体は常に有効）。直れば自作モデルは無料で+11ポイント見込める。

計測コード: llama-cpp-python で `U+EE02+文脈 / U+EE00+読み / U+EE01` を組み、
temperature=0 で生成して `expected_output` と完全一致を数えるだけ（辞書・制約なし）。

チェックポイント途中評価の例（学習の継続判断に使う）。チェックポイントには
トークナイザが保存されないため、基盤/出力先からコピーしてからGGUF化する:

```bash
CKPT=training/imellm-llmjp-150m/checkpoint-4000
cp training/base/llm-jp-3-150m/tokenizer* training/base/llm-jp-3-150m/special_tokens_map.json "$CKPT/"
cp training/iroha-llmjp-150m/added_tokens.json "$CKPT/" 2>/dev/null || true  # train.pyが出力していれば
training/convert-gguf.sh "$CKPT"   # → ~/Library/Application Support/iroha/models/checkpoint-4000-f16.gguf
scripts/bench-compare.sh \
  ~/Library/Application\ Support/iroha/models/zenz-v3.1-small-Q5_K_M.gguf \
  ~/Library/Application\ Support/iroha/models/checkpoint-4000-f16.gguf
```

段階的スケールアップの目安:
- まず100万件×2エポックで検証（A100級で1時間前後）→ bench で完全一致70%+ならパイプライン健全
- 全件3300万件は1エポック≒1.5〜2Bトークン（A100級で十数時間）。`--save-steps` の
  チェックポイントを途中でGGUF化してbenchすると学習継続の判断がしやすい
- `--from-scratch` でランダム初期化からのフルスクラッチ学習も可能（全件規模のデータ推奨）

検証済み事項（2026-08-30、学習前のGGUF疎通確認）:
- llm-jp-3-150m は vendor/llama.cpp (b10689) でそのまま変換・推論可能（パッチ不要）
- SentencePiece系なのでzenzで必要だったpre-tokenizerパッチは不要
- U+EE00〜02 は `add_special_tokens` で単一トークン化（ID 99574〜99576）。
  config の vocab_size 99,584 に余剰行があるため embedding resize も不要
- ZenzEngine は `parse_special=true` でトークナイズするため特殊トークンはそのまま動く
- transformers 5.x は追加トークンを tokenizer.json にしか保存しないため、train.py が
  `added_tokens.json` を書き出してGGUFに反映させる（USER_DEFINEDトークン化）
- 2000件のミニ学習でE2E疎通確認済み（学習→GGUF→bench）。f16のまま平均24.1ms/変換で
  レイテンシ目標50ms内

## 学習フォーマット（zenz互換）

```
[U+EE02左文脈] U+EE00カタカナ読み U+EE01変換結果
例: 天気予報によるとキショウ気象
```

このフォーマットを維持する限りZenzEngineはそのまま使える。フォーマット自体を
変える場合は、ConversionEngineに準拠した新しいエンジン実装を追加すればよい。

## 別アーキテクチャを試す場合の注意

- llama.cppが対応するアーキテクチャ（GPT-2, Llama, Qwen等）であればGGUF化して動く
- SentencePiece系（llm-jp等）は `tokenizer.model` をモデルディレクトリに置けば変換できる
  （train.py は基盤ディレクトリから出力先へ自動コピーする）
- BPE系でトークナイザのpre-tokenizer名が本家llama.cppに未登録の場合、
  [patches/llama-cpp-zenz-pretokenizer.patch](../patches/llama-cpp-zenz-pretokenizer.patch)
  と同様のマッピング追加が必要（`vendor/llama.cpp/src/llama-vocab.cpp`）
- 即時性の目安: ライブ変換は1操作50ms以内が目標。100M級（xsmall〜small）が現実的で、
  1B級はM1 Maxでもライブ変換にはやや重い
- ライセンス: llm-jp-3はApache-2.0。zenz-v2.5-datasetのライセンス（CC-BY-SA系）は
  学習済みモデルを配布する場合に要確認（個人利用・研究利用は問題なし）
