#!/bin/bash
# AJIMEE-Bench（zenzai公式のかな漢字変換ベンチマーク）の評価データを取得する。
# 出典: https://github.com/azooKey/AJIMEE-Bench （JWTD_v2/v1、200件）
# データのライセンスは日本語Wikipedia入力誤りデータセット(v2)に準拠し CC-BY-SA 3.0。
# リポジトリには含めず、評価時にこのスクリプトで取得する。
set -euo pipefail
# testdata/ はリポジトリルートにあるため、そこへ移動する
cd "$(dirname "$0")/../.."

DEST=testdata/ajimee/evaluation_items.json
mkdir -p testdata/ajimee
curl -fsSL -o "$DEST" \
  https://raw.githubusercontent.com/azooKey/AJIMEE-Bench/main/JWTD_v2/v1/evaluation_items.json

COUNT=$(python3 -c "import json; print(len(json.load(open('$DEST'))))")
echo "wrote $DEST (${COUNT}件)"
