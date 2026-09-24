# iroha typo ベンチマーク（200 件、2026-09-24）

typo normalizer（読み → 読みの打ち間違い訂正）を測るための、**iroha のために書き下ろした**
200 件。外部データに由来する文は含まない。

| ファイル | 中身 |
|---|---|
| `bench.tsv` | **正本**（手で編集する）。1 行 1 件 |
| `make_bench.py` | bench.tsv から下の 2 つを作り、中身を検査する |
| `typo_bench.jsonl` | 評価用（`iroha-cli typo eval` がそのまま読める） |
| `REVIEW.md` | 確認用の一覧（カテゴリ別の表） |

```sh
cd testdata/iroha/typo
../../../dataset/.venv/bin/python make_bench.py --corpus   # 作り直し + 検査（--corpus は学習コーパスとの重なり）
cd ../../..
macos/.build/release/iroha-cli typo eval testdata/iroha/typo/typo_bench.jsonl
```

## 作り方

- **入力は打鍵列から作る。** `typed`（実際に打ったローマ字）を iroha と同じ規則でかなに戻したものが
  `noisy`。解決できない打鍵がローマ字のまま残るのも iroha の挙動どおり（`しゅうまrすあいてる`）
- **誤りの型の割合は Komatsu & Nakatoh 2018**（ハードウェアキーボードの打ち間違い、Fig. 3）に寄せ、
  件数の少ない型も測れるよう各型に最低限の件数を置いた。論文で多いとされた取り違え
  （I↔O・I↔U・U↔O・R↔T・A↔O・SHA↔SHO・ん の nnn）と、JWTD の実誤りで多いもの
  （を⇄の = w↔n・が⇄か・d↔t・b↔p）を入れてある
- **日本語のローマ字入力に特有の誤り**: ん の n が 1 つ（`shashinokurune` → しゃしのくるね）、促音の過不足
- **文体は学習コーパスに足りない日常の入力**（チャット・ビジネスメール・SNS）を中心に、論文調・報道調を少し
- **誤りのない入力（60 件）は過剰訂正を測る**。正しい繰り返し（からから・ぐるぐる）、長音、方言、
  小さい仮名の外来語、typo に見える語（さど・きゅうきょ）、打ちかけの読みなど、訂正したくなる正しい入力を多めにした
- 長さは 4 文字以上（本体は 4 文字未満では訂正しない）。文字はモデルの語彙（vocab-120）に収めた
  （`？` `！` `「」` `〜` は語彙に無く、`iroha-cli typo eval` が素通しにするので使っていない）

| カテゴリ | 件数 | 論文の割合 |
|---|---:|---:|
| Replacement（隣接キー） `substitution` | 59 | Replacement 60.3% |
| Replacement（離れたキー） `key_far` | 14 | （同上に含まれる） |
| Insertion / Involvement `insertion` | 15 | 12.5% |
| Insertion / Other `insertion` | 5 | 4.2% |
| Insertion / Repetition `repeated_key` ほか | 14 | 9.4% |
| Removal `deletion` / `missing_double_consonant` | 21 | 11.5% |
| Exchange `transposition` | 8 | 2.2% |
| 2 つの誤り | 4 | — |
| 誤りなし `none` | 60 | — |

## 検査（make_bench.py）

- 誤りありは `noisy != clean`、誤りなしは `noisy == clean`
- 正しい読みの打鍵列と `typed` の距離（OSA）が誤りの数と一致する
- 語彙外の文字がない・4 文字以上・重複がない
- `--corpus`: 学習コーパス（dataset/data/typo-corpus）との重なり。
  **入力（noisy）が正しい読みとしてコーパスに現れるもの**は別の語として成り立つので避けた
  （わかりました → わかいました を差し替えた）。正しい読みが学習にも出てくるものは 68 件（よくある言い回し）

## 形式（typo_bench.jsonl）

```json
{"id":"del02","noisy":"しゃしのくるね","clean":"しゃしんおくるね","error_type":"deletion","category":"Removal","domain":"chat","surface":"写真送るね","typed":"shashinokurune","keys_clean":"shashinnokurune","keystroke_distance":1,"note":"ん の n が 1 つ（n+o が の になる）"}
```

`iroha-cli typo eval` が使うのは `noisy` / `clean` / `error_type`（誤りなしは `none`）だけ。

## 参考: 配布中のモデル（2026-09-24）

`iroha-cli typo eval`（θ = 2.0）: 訂正率 50.71%・過剰訂正率 0.00%。θ なし: 55.00%・5.00%。
