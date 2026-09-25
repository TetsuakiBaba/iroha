# iroha typo ベンチマーク（200 件、2026-09-24 作成・2026-09-25 改訂）

typo normalizer（読み → 読みの打ち間違い訂正）を測るための、**iroha のために書き下ろした**
200 件。外部データに由来する文は含まない。**学習データに含まれない**（正しい読みも誤りのある入力も、
typo normalizer の学習データの読みのどれとも一致しない。下の「検査」）。

**2026-09-25 の改訂**: 初版は 200 件中 121 件の正しい読みが学習データに入っていた（「ちょっとまってて」
「すみません」のような短い言い回しは、文節の読みとしてほぼ必ずコーパスに現れる）。その 121 件に場面に合う
語句を前後に足し、学習データに無い読みにした（例: ちょっとまってて → かいさつのまえでちょっとまってて）。
**誤りの打鍵（位置と種類）・誤りの型・カテゴリの件数・誤りなしで確かめたい性質（繰り返し・方言・長音・打ちかけ）
はそのまま**で、打鍵列は足した読みのローマ字を前後につないで作った。先頭・末尾の誤り（sub47・sub48）と
打ちかけ（cln59・cln60）は、その位置を保つ側にだけ足した。**初版と改訂版の数字は比べられない**。

| ファイル | 中身 |
|---|---|
| `bench.tsv` | **正本**（手で編集する）。1 行 1 件 |
| `make_bench.py` | bench.tsv から下の 2 つを作り、中身を検査する |
| `typo_bench.jsonl` | 評価用（`iroha-cli typo eval` がそのまま読める） |
| `REVIEW.md` | 確認用の一覧（カテゴリ別の表） |

```sh
cd testdata/iroha/typo
../../../dataset/iroha-typo-normalizer/.venv/bin/python make_bench.py --corpus   # 作り直し + 検査（--corpus は学習データとの重なり）
# readings-spoken はこのベンチの読みを除外して作るので、除外をかける前の canonical とも照合する
../../../dataset/iroha-typo-normalizer/.venv/bin/python make_bench.py --corpus --canonical <typo-spoken の data_dir>/canonical
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
- `--corpus`: **学習データに含まれない**こと（重なれば失敗）。照合するのは
  dataset/iroha-typo-normalizer/data/typo-corpus の readings-balanced / full / spoken の train・validation・test
  （文と文節の読み）。正しい読み（clean）が一致しないことと、誤りのある入力（noisy）が正しい読みとして
  現れないこと（現れれば別の語として成り立つ。初版で わかりました → わかいました を差し替えた）
- `--canonical <dir>`: canonical（文の読みと文節の読み）とも照合する。**readings-spoken は作るときにこのベンチの
  読みを除外している**（`config/typo-spoken.yaml`）ので、一覧だけでは重なりが見えない。除外をかける前の
  canonical と照合すること。2026-09-25 は、話し言葉 6 ソースの canonical（open2ch は `board_ratio` で間引いた後）と
  照合して 0 件。open2ch の間引きの割合を変えたら照合し直す

## 形式（typo_bench.jsonl）

```json
{"id":"del02","noisy":"しゃしのくるね","clean":"しゃしんおくるね","error_type":"deletion","category":"Removal","domain":"chat","surface":"写真送るね","typed":"shashinokurune","keys_clean":"shashinnokurune","keystroke_distance":1,"note":"ん の n が 1 つ（n+o が の になる）"}
```

`iroha-cli typo eval` が使うのは `noisy` / `clean` / `error_type`（誤りなしは `none`）だけ。

## 参考（改訂版、2026-09-25、`iroha-cli typo eval`）

| モデル | θ なし 訂正率 / 過剰訂正率 | θ = 2.0 訂正率 / 過剰訂正率 | θ = 4.0 訂正率 / 過剰訂正率 |
|---|---|---|---|
| small-v1（配布中、zenz データ） | 52.86% / 3.33% | 49.29% / 0.00% | 44.29% / 0.00% |
| small-v2 候補（readings-balanced 8 epoch） | 59.29% / 16.67% | 55.71% / 10.00% | 51.43% / 1.67% |

初版（2026-09-24）では small-v1 が θ = 2.0 で 50.71% / 0.00%、small-v2 候補が 60.71% / 5.00% だった。
small-v2 候補は初版の 59 件の正しい読みを学習に含んでいた。
