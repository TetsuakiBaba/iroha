#!/bin/bash
# 複数のGGUFモデルを同一条件で評価して比較表（Markdown）を出す。
# 評価は2本立て:
#   1. testdata/eval.tsv          … 独自評価セット（完全一致・CER）
#   2. AJIMEE-Bench (200件)       … zenzai公式ベンチマーク（acc@1・MinCER）
#
# 使い方:
#   scripts/bench-compare.sh <model1.gguf> [model2.gguf ...]
# 例（zenz と 学習済みllm-jp-3-150m の比較）:
#   scripts/bench-compare.sh \
#     ~/Library/Application\ Support/iroha/models/zenz-v3.1-small.gguf \
#     training/iroha-llmjp-150m/iroha-llmjp-150m-f16.gguf
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "使い方: $0 <model.gguf> [model.gguf ...]" >&2
    exit 1
fi

# 相対パスは呼び出し時のカレントディレクトリ基準で絶対パス化してから
# リポジトリルートへ移動する（cd後に解決すると別の場所を指してしまう）
MODELS=()
for MODEL in "$@"; do
    case "$MODEL" in
        /*) ;;
        *) MODEL="$PWD/$MODEL" ;;
    esac
    if [ ! -f "$MODEL" ]; then
        echo "エラー: モデルファイルが見つかりません: $MODEL" >&2
        exit 1
    fi
    MODELS+=("$MODEL")
done
set -- "${MODELS[@]}"

cd "$(dirname "$0")/.."

if [ ! -f testdata/ajimee/evaluation_items.json ]; then
    echo "==> AJIMEE-Benchデータを取得" >&2
    bash scripts/fetch-ajimee.sh >&2
fi

echo "==> iroha-cli をビルド" >&2
swift build -c release --product iroha-cli >&2
CLI=.build/release/iroha-cli

RESULTS=$(mktemp -d)
trap 'rm -rf "$RESULTS"' EXIT

TABLE="| モデル | eval.tsv 完全一致 | eval.tsv CER | AJIMEE acc@1 | AJIMEE MinCER | 平均レイテンシ |
|---|---|---|---|---|---|"

for MODEL in "$@"; do
    NAME=$(basename "$MODEL" .gguf)
    echo "==> $NAME : eval.tsv" >&2
    IROHA_MODEL="$MODEL" "$CLI" bench testdata/eval.tsv > "$RESULTS/$NAME.bench.txt" 2>/dev/null
    BENCH=$(tail -1 "$RESULTS/$NAME.bench.txt")
    echo "    $BENCH" >&2

    echo "==> $NAME : AJIMEE-Bench" >&2
    IROHA_MODEL="$MODEL" "$CLI" ajimee testdata/ajimee/evaluation_items.json > "$RESULTS/$NAME.ajimee.txt" 2>/dev/null
    AJIMEE=$(grep '^全体:' "$RESULTS/$NAME.ajimee.txt")
    echo "    $AJIMEE" >&2

    # 「件数: 40  完全一致: 36 (90.0%)  CER: 4.52%  平均: 29.1ms/変換」から抽出
    EXACT=$(echo "$BENCH" | sed -E 's/.*完全一致: [0-9]+ \(([0-9.]+)%\).*/\1%/')
    CER=$(echo "$BENCH" | sed -E 's/.*CER: ([0-9.]+)%.*/\1%/')
    # 「全体: acc@1 167/200 (83.5%)  MinCER 3.65%  平均 45.2ms/変換」から抽出
    ACC=$(echo "$AJIMEE" | sed -E 's|.*acc@1 ([0-9]+/[0-9]+) \(([0-9.]+)%\).*|\2% (\1)|')
    MINCER=$(echo "$AJIMEE" | sed -E 's/.*MinCER ([0-9.]+)%.*/\1%/')
    MS=$(echo "$AJIMEE" | sed -E 's|.*平均 ([0-9.]+)ms/変換.*|\1ms|')

    TABLE="$TABLE
| $NAME | $EXACT | $CER | $ACC | $MINCER | $MS |"
done

echo
echo "$TABLE"
echo
echo "（誤答の内訳は各モデルの実行ログ参照。再実行: IROHA_MODEL=<gguf> $CLI ajimee testdata/ajimee/evaluation_items.json）"
