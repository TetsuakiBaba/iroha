#!/bin/bash
# 複数のGGUFモデルを同一条件で評価して比較表（Markdown）を出す。
# 評価は2本立て:
#   1. testdata/eval.tsv          … 独自評価セット（完全一致・CER）。NNのみ
#   2. AJIMEE-Bench (200件)       … zenzai公式ベンチマーク（acc@1・MinCER）。
#                                   「NNのみ」(IROHA_LATTICE=off) と
#                                   「辞書ラティス + NN再採点」(IROHA_LATTICE=always) の2条件
# ユーザ辞書・学習は iroha-cli の bench / ajimee の既定どおり空（モデルの素の力を測る）。
# IME本体のデータを混ぜて測りたいときは IROHA_WITH_USER_DATA=1 を付けて呼ぶ。
# 標準条件・基準値は training/README.md「比較の標準条件」を参照。
#
# 使い方（リポジトリルートから）:
#   macos/scripts/bench-compare.sh <model1.gguf[:adapter.gguf]> [model2.gguf[:adapter.gguf] ...]
#   `:adapter.gguf` を付けると追加学習の LoRA アダプタ（iroha-train の出力）を適用して測る
#   （IROHA_LORA）。同じベースをアダプタ有無で並べれば追加学習の効果と忘却が1表で見える
# 例（zenz と 学習済みllm-jp-3-150m の比較）:
#   macos/scripts/bench-compare.sh \
#     ~/Library/Application\ Support/iroha/models/zenz-v3.1-small.gguf \
#     training/iroha-llmjp-150m/iroha-llmjp-150m-f16.gguf
# 例（ベース vs ベース+LoRA）:
#   macos/scripts/bench-compare.sh zenz.gguf zenz.gguf:~/Library/Application\ Support/iroha/models/adapters/x.gguf
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "使い方: $0 <model.gguf[:adapter.gguf]> [model.gguf[:adapter.gguf] ...]" >&2
    exit 1
fi

# 相対パスは呼び出し時のカレントディレクトリ基準で絶対パス化してから
# パッケージルート(macos/)へ移動する（cd後に解決すると別の場所を指してしまう）
absolutize() {
    case "$1" in
        /*) echo "$1" ;;
        *) echo "$PWD/$1" ;;
    esac
}
MODELS=()
for SPEC in "$@"; do
    MODEL="${SPEC%%:*}"
    ADAPTER=""
    case "$SPEC" in *:*) ADAPTER="${SPEC#*:}" ;; esac
    MODEL=$(absolutize "$MODEL")
    if [ ! -f "$MODEL" ]; then
        echo "エラー: モデルファイルが見つかりません: $MODEL" >&2
        exit 1
    fi
    if [ -n "$ADAPTER" ]; then
        ADAPTER=$(absolutize "$ADAPTER")
        if [ ! -f "$ADAPTER" ]; then
            echo "エラー: アダプタファイルが見つかりません: $ADAPTER" >&2
            exit 1
        fi
        MODELS+=("$MODEL:$ADAPTER")
    else
        MODELS+=("$MODEL")
    fi
done
set -- "${MODELS[@]}"

cd "$(dirname "$0")/.."

if [ ! -f ../testdata/ajimee/evaluation_items.json ]; then
    echo "==> AJIMEE-Benchデータを取得" >&2
    bash scripts/fetch-ajimee.sh >&2
fi

echo "==> iroha-cli をビルド" >&2
swift build -c release --product iroha-cli >&2
CLI=.build/release/iroha-cli

RESULTS=$(mktemp -d)
trap 'rm -rf "$RESULTS"' EXIT

TABLE="| モデル | eval.tsv 完全一致 | eval.tsv CER | AJIMEE acc@1 (NNのみ) | MinCER | ms | AJIMEE acc@1 (辞書+NN) | MinCER | ms |
|---|---|---|---|---|---|---|---|---|"

# 「全体: acc@1 167/200 (83.5%)  MinCER 3.65%  平均 45.2ms/変換」から3列分を抽出
ajimee_columns() {
    local LINE="$1"
    local ACC MINCER MS
    ACC=$(echo "$LINE" | sed -E 's|.*acc@1 ([0-9]+/[0-9]+) \(([0-9.]+)%\).*|\2% (\1)|')
    MINCER=$(echo "$LINE" | sed -E 's/.*MinCER ([0-9.]+)%.*/\1%/')
    MS=$(echo "$LINE" | sed -E 's|.*平均 ([0-9.]+)ms/変換.*|\1ms|')
    echo "$ACC | $MINCER | $MS"
}

for SPEC in "$@"; do
    MODEL="${SPEC%%:*}"
    ADAPTER=""
    case "$SPEC" in *:*) ADAPTER="${SPEC#*:}" ;; esac
    NAME=$(basename "$MODEL" .gguf)
    if [ -n "$ADAPTER" ]; then NAME="$NAME+$(basename "$ADAPTER" .gguf)"; fi
    export IROHA_LORA="$ADAPTER"
    echo "==> $NAME : eval.tsv (NNのみ)" >&2
    IROHA_MODEL="$MODEL" IROHA_LATTICE=off "$CLI" bench ../testdata/eval.tsv > "$RESULTS/$NAME.bench.txt" 2>/dev/null
    BENCH=$(tail -1 "$RESULTS/$NAME.bench.txt")
    echo "    $BENCH" >&2

    echo "==> $NAME : AJIMEE-Bench (NNのみ)" >&2
    IROHA_MODEL="$MODEL" IROHA_LATTICE=off "$CLI" ajimee ../testdata/ajimee/evaluation_items.json > "$RESULTS/$NAME.ajimee-off.txt" 2>/dev/null
    AJIMEE_OFF=$(grep '^全体:' "$RESULTS/$NAME.ajimee-off.txt")
    echo "    $AJIMEE_OFF" >&2

    echo "==> $NAME : AJIMEE-Bench (辞書ラティス + NN再採点)" >&2
    IROHA_MODEL="$MODEL" IROHA_LATTICE=always "$CLI" ajimee ../testdata/ajimee/evaluation_items.json > "$RESULTS/$NAME.ajimee-always.txt" 2>/dev/null
    AJIMEE_ALWAYS=$(grep '^全体:' "$RESULTS/$NAME.ajimee-always.txt")
    echo "    $AJIMEE_ALWAYS" >&2

    # 「件数: 40  完全一致: 36 (90.0%)  CER: 4.52%  平均: 29.1ms/変換」から抽出
    EXACT=$(echo "$BENCH" | sed -E 's/.*完全一致: [0-9]+ \(([0-9.]+)%\).*/\1%/')
    CER=$(echo "$BENCH" | sed -E 's/.*CER: ([0-9.]+)%.*/\1%/')

    TABLE="$TABLE
| $NAME | $EXACT | $CER | $(ajimee_columns "$AJIMEE_OFF") | $(ajimee_columns "$AJIMEE_ALWAYS") |"
done

echo
echo "$TABLE"
echo
echo "（誤答の内訳は各モデルの実行ログ参照。再実行: IROHA_MODEL=<gguf> [IROHA_LORA=<adapter>] IROHA_LATTICE=off|always $CLI ajimee ../testdata/ajimee/evaluation_items.json）"
