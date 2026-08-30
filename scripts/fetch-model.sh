#!/bin/bash
# zenz-v3.1-small (GGUF, Q5_K_M, 約72MB) をダウンロードする
set -euo pipefail

MODEL_DIR="$HOME/Library/Application Support/iroha/models"
MODEL_FILE="zenz-v3.1-small-Q5_K_M.gguf"
URL="https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf/resolve/main/ggml-model-Q5_K_M.gguf"

mkdir -p "$MODEL_DIR"
if [ -f "$MODEL_DIR/$MODEL_FILE" ]; then
    echo "既にダウンロード済み: $MODEL_DIR/$MODEL_FILE"
    exit 0
fi

echo "==> zenz-v3.1-small をダウンロード中..."
curl -L --progress-bar "$URL" -o "$MODEL_DIR/$MODEL_FILE.tmp"
mv "$MODEL_DIR/$MODEL_FILE.tmp" "$MODEL_DIR/$MODEL_FILE"
echo "==> 完了: $MODEL_DIR/$MODEL_FILE"
echo "ライセンス: CC-BY-SA-4.0 (https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf)"
