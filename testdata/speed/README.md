# 変換速度の計測（macOS・Windows・Linux 共通）

かな漢字変換モデルの変換時間を、3 つの OS で同じ手順・同じ集計で測るための手順。
変換の正しさ（acc@1）と同じ実行で取れるので、精度の比較（`../../training/README.md` の「比較の標準条件」）と一緒に回してよい。

## 何を測るか

- `iroha-cli ajimee <evaluation_items.json>` が 1 件ずつ変換し、**読み 1 つを変換 1 回に要した時間**（ミリ秒）を測る。
  モデルの読み込みは最初のウォームアップで済ませ、計測には入らない
- 評価セットは **iroha-bench-v1**（`../iroha-bench-v1/evaluation_items.json`、3,253 件。読みは 8〜40 文字、文脈つきが 1,109 件）。
  手早く見るだけなら AJIMEE（`../ajimee/evaluation_items.json`、200 件）でもよいが、件数が少なく長い読みが少ない
- 環境変数 `IROHA_TIMINGS=<TSV のパス>` で、1 件ごとの時間を TSV に書き出す。先頭の 4 列は 3 つの OS で同じ:

  | 列 | 中身 |
  |---|---|
  | `index` | 評価セットの項目の番号 |
  | `reading_length` | 読みの文字数 |
  | `context_length` | 左文脈の文字数 |
  | `ms` | 変換 1 回の時間（ミリ秒） |

  後ろに NN の内訳の列が付く。macOS は `prompt_ms`・`decode_ms`・`decode_steps`・`select_ms`、
  Windows・Linux は `nn_ms`・`nn_calls`（NN の計算の合計と回数）
- 集計は `summarize.py`（このディレクトリ。標準ライブラリだけ）。平均・中央値・90/99 パーセンタイル・最大・
  NN の割合と、読みの文字数別の中央値を Markdown の表で出す。**平均だけで比べない**（長い読みの数件で平均が動く）

macOS版はアプリの中でも同じ計測ができる（設定 > モデル > 変換のベンチマーク、開発者モード。2026-11-14）。
採点と集計は `iroha-cli ajimee` と同じ実装で、条件（ユーザ辞書・学習を使わない、`off` / `always`）も同じ。
1 件ごとの時間は `<データフォルダ>/logs/bench/<日時>-<ホスト名>.tsv` に上の列のまま書くので、`summarize.py` にそのまま渡せる。
記録の `.json` には機種・CPU・OS・電源・発熱の状態・推論の実行先とスレッド数・計測中のキー入力の有無も残る。

## アプリ内のベンチマークの記録（3 つの OS で同じ形）

macOS版の形が正（`macos/Sources/IrohaCore/Benchmark/BenchmarkRun.swift`。採点と集計は同じディレクトリの `Benchmark.swift`・
`BenchmarkSummary.swift`）。Windows版・Linux版を作るときはこの形に合わせ、記録どうしを同じ表に並べられるようにする。

- 置き場所: `<データフォルダ>/logs/bench/<yyyyMMdd-HHmmss>-<ホスト名>.json` と同じ名前の `.tsv`。
  ホスト名の英数字とハイフン以外は `_` にする。日時は計測を始めた時刻（その機械の地方時）
- `.tsv`: 先頭の 4 列（`index reading_length context_length ms`）は 3 つの OS で同じ。後ろは各 OS の `iroha-cli ajimee` の
  `IROHA_TIMINGS` と同じ列（macOS は `prompt_ms`・`decode_ms`・`decode_steps`・`select_ms`、Windows・Linux は `nn_ms`・`nn_calls`）
- 評価セットの取得: カタログ `models/benchmarks.json`（`formatVersion` 1）から落とし、大きさと SHA-256 を照合してから
  `<データフォルダ>/benchmarks/<ID>.jsonl` に置き、隣の `<ID>.json` に素性（`id`・`name`・`items`・`sha256`・`license`・
  `sources`・`notice`・`installedAt`）を書く
- 条件はユーザ辞書・学習・自分の原稿の表記・専門辞書を使わない。`off`（モデルだけ）と `always`（辞書ラティス + 再採点）。
  最初の 1 件をモデルの読み込みを含めて変換し（`warmUpMilliseconds`）、その時間は集計に入れない

`.json` のキー（日時は ISO 8601。値が取れない項目はキーごと書かない。推し量った値を入れない）:

| キー | 中身 |
|---|---|
| `formatVersion` | 2（このキーが無い記録は形式 1。2026-11-14 の macOS版の最初の版で、`environment.chip`・`lowPowerMode` という名前だった） |
| `id` | ファイル名（拡張子なし） |
| `benchmarkID` / `benchmarkName` | 評価セットの ID と表示名 |
| `startedAt` / `finishedAt` | 計測の始めと終わり |
| `warmUpMilliseconds` | モデルの読み込みを含む最初の変換の時間 |
| `inputDuringRun` | 計測中にキーボードの入力があったか。確かめられない環境では書かない |
| `condition.lattice` | `off` / `always` |
| `condition.backend` | `llama` / `mlx` |
| `condition.model` / `condition.adapter` | モデル・LoRA アダプタのファイル名 |
| `condition.device` | 推論の実行先。`CPU` か「ggml の装置名: 説明」（`MTL0: Apple M2`・`Vulkan0: NVIDIA GeForce RTX 4060` など。GPU に載せる層が 0 でなく、種類が GPU か IGPU の最初の装置）。MLX は `MLX: <チップ名>` |
| `condition.threads` | llama.cpp の生成のスレッド数（`llama_n_threads`） |
| `environment.platform` | `macos` / `windows` / `linux` |
| `environment.appVersion` / `hostName` | アプリの版・ホスト名 |
| `environment.machineModel` | 機種（下の表） |
| `environment.cpu` / `cpuCores` | CPU の名前・論理コアの数 |
| `environment.osVersion` | 下の表 |
| `environment.powerSource` | `ac` / `battery` |
| `environment.powerSaving` | 省電力の設定が効いていたか |
| `environment.thermalStateBefore` / `After` | 発熱の状態（`nominal` / `fair` / `serious` / `critical`） |
| `summary` | `total`・`withContext`・`withoutContext`（各 `count`・`hits`・`minCERSum`）、`perSource`（`source`・`tally`）、`latency`（`count`・`mean`・`p50`・`p90`・`p99`・`max`）、`lengthBins`（`lower`・`upper`・`count`・`p50`。項目の無い区切りは書かない。最後の区切りは `upper` なし）、`neuralNetworkShare`（NN の時間の合計 ÷ 変換の時間の合計） |
| `misses` | 正解しなかった項目（`index`・`context`・`input`・`output`・`expected`） |

OS ごとの取り方:

| 項目 | macOS | Windows | Linux |
|---|---|---|---|
| `machineModel` | sysctl `hw.model` | SMBIOS の製造元と製品名（`HKLM\HARDWARE\DESCRIPTION\System\BIOS` の `SystemManufacturer`・`SystemProductName`） | `/sys/class/dmi/id/sys_vendor` と `product_name` |
| `cpu` | sysctl `machdep.cpu.brand_string` | `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0` の `ProcessorNameString` | `/proc/cpuinfo` の `model name` |
| `cpuCores` | `ProcessInfo.activeProcessorCount` | `GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)` | `sysconf(_SC_NPROCESSORS_ONLN)` |
| `osVersion` | 「macOS 27.0 (26A428)」 | 「Windows 11 24H2 (26100)」（`RtlGetVersion` のビルド番号と `DisplayVersion`） | `/etc/os-release` の `PRETTY_NAME` |
| `powerSource` | `IOPSGetProvidingPowerSourceType` | `GetSystemPowerStatus` の `ACLineStatus` | `/sys/class/power_supply/*/online`（種類 `Mains`） |
| `powerSaving` | 低電力モード | `GetSystemPowerStatus` の `SystemStatusFlag`（省電力モード） | power-profiles-daemon の `power-saver`。無ければ書かない |
| `thermalState…` | `ProcessInfo.thermalState` | 書かない | 書かない |
| `inputDuringRun` | `CGEventSource.secondsSinceLastEventType`（keyDown）が計測の時間より短い | `GetLastInputInfo`（マウスも含む） | X11 は XScreenSaver の idle。Wayland では書かない |

## そろえる条件

- **2 条件で測る**: `IROHA_LATTICE=off`（モデルだけ。ライブ変換の第一候補と同じ経路）と
  `IROHA_LATTICE=always`（辞書ラティスの候補をモデルで採点し直す。候補ウィンドウに近い）。既定（指定なし）は
  候補数 1 なので `off` と同じ
- モデルは配布している量子化（Q5_K_M）。比べるモデルは同じ機械・同じビルドで、**1 つずつ順に**測る（同時に走らせると CPU を取り合う）
- ユーザ辞書・学習は `ajimee` が既定で空にする（`IROHA_WITH_USER_DATA` を付けない）
- スレッド数は既定のまま（llama.cpp の既定。Windows・Linux は `IROHA_THREADS` で変えられる）。変えたら記録に書く
- 最適化したビルドで測る（macOS は `-c release`、Windows は `build-tip.ps1` の RelWithDebInfo、Linux は既定の Release）
- 計測中はほかの重い処理（ビルド・学習・同期）をしない。ノート PC は電源につなぐ。リモートデスクトップで操作している
  機械は、計測中に画面を動かさない

## 手順

モデルのパスは各 OS のデータフォルダの `models/`（macOS `~/Library/Application Support/iroha/models`、
Windows `%LOCALAPPDATA%\iroha\models`、Linux `~/.local/share/iroha/models`）。データフォルダを変えている機械はそちら。

### macOS

```sh
cd macos && swift build --build-system native -c release --product iroha-cli
M="$HOME/Library/Application Support/iroha/models"
for lattice in off always; do
  for model in iroha-kkc-v1-Q5_K_M zenz-v3.1-small-Q5_K_M; do
    IROHA_MODEL="$M/$model.gguf" IROHA_LATTICE=$lattice IROHA_TIMINGS="/tmp/speed/$model-$lattice.tsv" \
      .build/release/iroha-cli ajimee ../testdata/iroha-bench-v1/evaluation_items.json > "/tmp/speed/$model-$lattice.log"
  done
done
```

推論エンジンを MLX にするときは `IROHA_BACKEND=mlx`（T5 のみ。CLAUDE.md の「かな漢字変換の推論」）。

### Windows（PowerShell）

```powershell
powershell -ExecutionPolicy Bypass -File windows\scripts\build-tip.ps1
$M = "$env:LOCALAPPDATA\iroha\models"
foreach ($lattice in "off", "always") {
  foreach ($model in "iroha-kkc-v1-Q5_K_M", "zenz-v3.1-small-Q5_K_M") {
    $env:IROHA_MODEL = "$M\$model.gguf"; $env:IROHA_LATTICE = $lattice
    $env:IROHA_TIMINGS = "$env:TEMP\speed\$model-$lattice.tsv"
    windows\build\iroha-cli\iroha-cli.exe ajimee testdata\iroha-bench-v1\evaluation_items.json > "$env:TEMP\speed\$model-$lattice.log"
  }
}
```

`IROHA_TIMINGS` のパスは英数字だけにする（Windows では環境変数を ANSI の文字コードで読む）。

### Linux

```sh
cmake -S linux -B linux/build -G Ninja && cmake --build linux/build
M="$HOME/.local/share/iroha/models"
for lattice in off always; do
  for model in iroha-kkc-v1-Q5_K_M zenz-v3.1-small-Q5_K_M; do
    IROHA_MODEL="$M/$model.gguf" IROHA_LATTICE=$lattice IROHA_TIMINGS="/tmp/speed/$model-$lattice.tsv" \
      linux/build/iroha-cli/iroha-cli ajimee testdata/iroha-bench-v1/evaluation_items.json > "/tmp/speed/$model-$lattice.log"
  done
done
```

### 集計

```sh
python3 testdata/speed/summarize.py \
  "iroha モデルのみ=/tmp/speed/iroha-kkc-v1-Q5_K_M-off.tsv" \
  "zenz モデルのみ=/tmp/speed/zenz-v3.1-small-Q5_K_M-off.tsv" \
  "iroha ラティス=/tmp/speed/iroha-kkc-v1-Q5_K_M-always.tsv" \
  "zenz ラティス=/tmp/speed/zenz-v3.1-small-Q5_K_M-always.tsv"
```

正しさ（acc@1）は各 `.log` の最後の「全体:」の行。

## 記録

結果はこの節に足す（新しいものを上に）。機械（CPU・メモリ）・OS・ビルド・スレッド数・コミットを必ず書く。

### 2026-11-14 Windows: iroha-kkc-v1 と zenz-v3.1-small（iroha-bench-v1）

- 機械: Intel Core i7-1065G7（4 コア 8 スレッド、ノート PC）、メモリ 16GB、Windows 11 Pro。リモートデスクトップで操作中の機械
- ビルド: `build-tip.ps1`（RelWithDebInfo、llama.cpp は CPU 版 AVX2）、コミット 2fe90ba。スレッド数は既定（llama.cpp の既定 4）
- モデル: どちらも Q5_K_M。4 条件を 1 つずつ順に測った（全体で約 61 分）
- NN の割合の列は、`IROHA_TIMINGS` を足す前のビルドで測ったので無い（同じビルドで AJIMEE 200 件を iroha-kkc-v1・
  ラティス再採点で測ると 95%）

| 条件 | 件数 | 平均 | 中央値 | 90% | 99% | 最大 | acc@1 |
|---|---:|---:|---:|---:|---:|---:|---:|
| iroha-kkc-v1 モデルのみ | 3253 | 173.4 | 142.7 | 315.1 | 507.9 | 2308.1 | 2252 (69.2%) |
| zenz-v3.1-small モデルのみ | 3253 | 171.2 | 149.4 | 282.0 | 425.8 | 1138.3 | 2216 (68.1%) |
| iroha-kkc-v1 ラティス再採点 | 3253 | 270.7 | 239.4 | 440.7 | 652.9 | 1455.2 | 2254 (69.3%) |
| zenz-v3.1-small ラティス再採点 | 3253 | 514.5 | 461.0 | 823.3 | 1198.9 | 1791.0 | 2222 (68.3%) |

読みの文字数別の中央値（括弧内は件数）:

| 条件 | 1〜10 | 11〜20 | 21〜30 | 31〜40 |
|---|---:|---:|---:|---:|
| iroha-kkc-v1 モデルのみ | 75.8 (485) | 119.9 (1763) | 197.7 (657) | 303.2 (348) |
| zenz-v3.1-small モデルのみ | 89.9 (485) | 131.7 (1763) | 203.7 (657) | 268.0 (348) |
| iroha-kkc-v1 ラティス再採点 | 142.0 (485) | 212.9 (1763) | 329.1 (657) | 457.9 (348) |
| zenz-v3.1-small ラティス再採点 | 289.1 (485) | 423.2 (1763) | 645.6 (657) | 856.6 (348) |

- モデルのみでは、中央値は iroha-kkc-v1 が 4% 速い（142.7 / 149.4ms）。読みが 20 文字以下では iroha-kkc-v1 が速く、
  31〜40 文字では zenz-v3.1-small が速い（303.2 / 268.0ms）。90%・99% も zenz-v3.1-small のほうが小さい
- ラティス再採点では、iroha-kkc-v1 が中央値で約半分（239.4 / 461.0ms）。候補の採点は 1 つずつ行う
  （`windows/README.md` の「変換モデル」節）ので、候補ごとの計算が軽いデコーダ 2 層の T5 が有利になる
- iroha-kkc-v1 モデルのみの最大 2308ms は 1 件だけ（llmjp_kaken-0233、読み 18 文字）。次に遅いものは 811ms で、
  計測中の一時的な遅れとみられる（測り直していない）
