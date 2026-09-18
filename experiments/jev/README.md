# jev 方式（選択肢ラベルのロジット直読み）でかな漢字変換候補を判定する実験（2026-09-18）

## 問い

TypeSafe AI の Jev（System One model: テキストを生成せず、状態＋型付きの問いを1回の
forward pass で読み、選択肢の確率を返す）を、iroha の「辞書ラティスの候補から正しい
組み合わせを選ぶ」段に使えるか。OpenJev（Qwen 4B などで Jev の I/F を再現）は
「選択肢ラベルのトークンのロジットを直読み」で実装しているので、同じ方式を試す。

## 手順

1. `iroha-cli ajimee-dump` で AJIMEE-Bench 200 件の候補プールを JSONL に書き出す
   （辞書ラティスの読み一致 n-best 10 件 ＋ zenz 生成 ＋ zenz の対数確率）
2. `jev_judge.py` で汎用 LLM（GGUF, llama.cpp）に判定させる。3 方式:
   - `choice`: 「文脈・読み・候補 A〜J」を見せ、次トークンのラベル A〜J のロジットで選ぶ（jev の choice）
   - `yesno`: 候補ごとに「正しいか 1/0」のロジット差で採点（jev の noul）
   - `seqlp`: 左文脈に続く候補文の系列対数確率（zenz の再採点と同じ方式を汎用 LLM でやる。jev ではない比較用）

```sh
cd macos && swift build -c release --product iroha-cli
IROHA_MODEL=../training/zenz-v3.1-small-Q5_K_M.gguf .build/release/iroha-cli ajimee-dump \
    ../testdata/ajimee/evaluation_items.json ../experiments/jev/ajimee-candidates.jsonl --lattice 10
cd .. && .venv/bin/python experiments/jev/jev_judge.py <model.gguf> experiments/jev/ajimee-candidates.jsonl \
    --mode choice --chat --drop-kana [--shuffle 1] [--verbose] [--out results/x.jsonl]
```

`--drop-kana` は「読みをそのまま仮名にした候補」（無変換）をプールから外す。付けないと
小さいモデルは読みと同じ見た目の候補を選んでしまう（1.5B で 10%）。

## 結果（AJIMEE-Bench 200 件、M1 Max、GGUF Q4_K_M、2026-09-18）

候補プール = 辞書ラティス n-best 10 ＋ zenz 生成（無変換の仮名候補は除く）。
プールに正解が含まれる上限（oracle）は **177/200 (88.5%)**。辞書ラティスだけなら 138/200 (69.0%)
（n-best を 30 に広げても 141/200）。参考の zenz-v3.1-small: 生成 171/200 (85.5%)、再採点 169/200 (84.5%)、
生成 42ms・採点 20ms。

| 判定モデル | choice（jev の choice） | choice・候補順シャッフル | yesno（jev の noul） | seqlp（系列対数確率、比較用） | choice の1件あたり時間 |
|---|---|---|---|---|---|
| Qwen2.5-1.5B-Instruct | 70 (35.0%) | 82 (41.0%) | 27.5% (40件) | 77.5% (40件) | 107ms |
| Sarashina2.2-3B-Instruct（日本語特化） | 117 (58.5%, chat なし) / 109 (54.5%, chat) | — | — | 153 (76.5%) | 155ms |
| Qwen3-4B-Instruct-2507 | 97 (48.5%) | 115 (57.5%) | 120 (60.0%) | 163 (81.5%) | 283ms（yesno 1015ms・seqlp 427ms） |
| Qwen2.5-7B-Instruct | 110 (55.0%) | 114 (57.0%) | 129 (64.5%) | 166 (83.0%) | 466ms（yesno 1675ms・seqlp 605ms） |

- アンサンブル（zenz の対数確率 ＋ 0.5 × 汎用 LLM の seqlp）: Qwen3-4B で 173/200、Qwen2.5-7B で 175/200。
  zenz 生成 171 に対し +2〜4 件で 200 件のノイズ幅（±5pt）の内側。追加コストは 400〜600ms
- choice 方式の確信度（softmax 後の p）は当たっていない: Qwen3-4B で p≥0.8 が 175/198 件、その正解率 51%。
  RLCD のような較正の学習なしにラベルのロジットを読んでも「確率」にはならない
- choice 方式には位置バイアス（B・C に集まる）が残り、シャッフルで数値が動く（4B: 48.5% ↔ 57.5%）
- seqlp が zenz より落ちる原因は読みを見ていないこと: 「酒類→種類」「意外→以外」「制作→政策」「矩形状→ク形状」など
  頻度の高い同音語に流れる（壊した 13 件 / 拾った 5 件、Qwen3-4B）。zenz は読み条件付きで学習しているのでここが強い

## 結論

1. **jev の「選択肢から1回で選ぶ」方式は、このタスクでは汎用 LLM（1.5B〜7B）で zenz に遠く及ばない**
   （最高 64.5% vs zenz 85.5%）。かな漢字変換の正誤判定は「読みと各候補の対応を文字単位で照合する」
   作業で、1 回の forward pass の末尾トークンの分布に押し込むには構造が細かすぎる。
   モデルを 1.5B → 7B にしても 41% → 57% で伸びが鈍く、数十 B に上げても zenz を超える見込みは薄い
2. **上限が低い**: 判定器が完璧でも、候補プール（ラティス n-best ＋ zenz 生成）に正解があるのは 88.5%。
   辞書ラティスだけなら 69%。「辞書ラティスで候補を作り、jev で選ぶ」構成は zenz の生成を候補に混ぜないと
   成立せず、混ぜるなら zenz を動かす時間はそのまま残る
3. **速度でも勝てない**: 4B で 1 件 283ms（プロンプト 227 トークンのプリフィル）。zenz-small の生成＋採点 62ms より
   4〜5 倍遅い。Jev 本体の 70〜500ms はサーバ GPU での値で、ローカル IME の目標（<50ms）には届かない
4. 汎用 LLM が効くのは seqlp（左文脈に続く文としての自然さ）で、zenz とのアンサンブルで +2〜4 件。
   これは jev ではなく「一般 LM による再採点」で、費用対効果（+1〜2pt / +500ms）は悪い

## jev の考え方で iroha に活かせる点

- 「生成せず 1 パスで読んで判定する」自体は iroha の再採点（`ZenzEngine.score`: プロンプト 1 回 ＋ 候補を
  系列ごとに並べた 1 バッチの teacher forcing）が既にやっている。iroha の再採点は jev 的に言えば
  「候補ごとの score 問い」を zenz で実装したもの
- 効きそうなのは **読みを条件に含めた専用の判定器**（zenz の系列尤度がそれ）か、zenz 自体を
  「候補 A〜J → ラベル」の形式で追加学習することだが、後者は系列尤度より情報を捨てるので利点が見えない
- 信頼度の較正（RLCD）は別問題として価値がある。誤変換らしい箇所の強調（2026-09-14 に試験→revert）で
  使ったマージン（`ConversionConfidence`）を較正する方向はあり得る

## ファイル

- `ajimee-candidates.jsonl` / `ajimee-candidates-n30.jsonl`: `iroha-cli ajimee-dump` の出力（n-best 10 / 30）
- `jev_judge.py`: 判定スクリプト（choice / yesno / seqlp）
- `results/*.jsonl`: 各モデル・各方式の判定結果（`--out`）。`results/rest.log`: 実行ログ
- モデルは HF キャッシュ（`~/.cache/huggingface/hub/`）に置いた。リポジトリには含めない
