#!/bin/bash
# 既定の変換モデル iroha-t5-alpha (GGUF, Q8_0, 約121MB) をダウンロードする
# （アプリも初回起動時に同じものを取得する。ModelDownloader.swift と同じ配布元・照合値）
set -euo pipefail

MODEL_DIR="$HOME/Library/Application Support/iroha/models"
MODEL_FILE="iroha-t5-alpha-Q8_0.gguf"
URL="https://github.com/TetsuakiBaba/iroha/releases/download/kkc-model-v1/iroha-t5-alpha-Q8_0.gguf"
SHA256="262baeb22a1ce640dc435eab217137809bec935cc663ae91e65a0458e8e7599b"

mkdir -p "$MODEL_DIR"
if [ -f "$MODEL_DIR/$MODEL_FILE" ]; then
    echo "既にダウンロード済み: $MODEL_DIR/$MODEL_FILE"
    exit 0
fi

echo "==> iroha-t5-alpha をダウンロード中..."
curl -fL --progress-bar "$URL" -o "$MODEL_DIR/$MODEL_FILE.tmp"
if [ "$(shasum -a 256 "$MODEL_DIR/$MODEL_FILE.tmp" | cut -d' ' -f1)" != "$SHA256" ]; then
    rm -f "$MODEL_DIR/$MODEL_FILE.tmp"
    echo "error: ダウンロードしたファイルの SHA-256 が一致しません" >&2
    exit 1
fi
mv "$MODEL_DIR/$MODEL_FILE.tmp" "$MODEL_DIR/$MODEL_FILE"
echo "==> 完了: $MODEL_DIR/$MODEL_FILE"
echo "ライセンス: CC BY-SA 4.0 (https://github.com/TetsuakiBaba/iroha/releases/tag/kkc-model-v1)"
