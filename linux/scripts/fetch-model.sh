#!/bin/bash
# 既定の変換モデル iroha-kkc-v1 (GGUF, Q5_K_M, 約82MB) を Linux のデータフォルダへダウンロードする。
# 配布元・照合値は macOS版の ModelDownloader.swift・macos/scripts/fetch-model.sh と同じ
# （既定のモデルを差し替えるときは、これも合わせる）。
# 置き場所: IROHA_DATA_DIR > $XDG_DATA_HOME/iroha > ~/.local/share/iroha の下の models/
set -euo pipefail

DATA_DIR="${IROHA_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/iroha}"
MODEL_DIR="$DATA_DIR/models"
MODEL_FILE="iroha-kkc-v1-Q5_K_M.gguf"
URL="https://github.com/TetsuakiBaba/iroha/releases/download/iroha-kkc-v1/iroha-kkc-v1-Q5_K_M.gguf"
SHA256="f060e259dbed28754f71e1634bc01a5d9d735d519f8ff72c333cac77c859efda"

mkdir -p "$MODEL_DIR"
if [ -f "$MODEL_DIR/$MODEL_FILE" ]; then
    echo "既にダウンロード済み: $MODEL_DIR/$MODEL_FILE"
    exit 0
fi

echo "==> iroha-kkc-v1 をダウンロード中..."
if command -v curl >/dev/null; then
    curl -fL --progress-bar "$URL" -o "$MODEL_DIR/$MODEL_FILE.tmp"
else
    wget -q --show-progress -O "$MODEL_DIR/$MODEL_FILE.tmp" "$URL"
fi
if [ "$(sha256sum "$MODEL_DIR/$MODEL_FILE.tmp" | cut -d' ' -f1)" != "$SHA256" ]; then
    rm -f "$MODEL_DIR/$MODEL_FILE.tmp"
    echo "error: ダウンロードしたファイルの SHA-256 が一致しません" >&2
    exit 1
fi
mv "$MODEL_DIR/$MODEL_FILE.tmp" "$MODEL_DIR/$MODEL_FILE"
echo "==> 完了: $MODEL_DIR/$MODEL_FILE"
echo "ライセンス: CC BY-SA 4.0 (https://github.com/TetsuakiBaba/iroha/releases/tag/iroha-kkc-v1)"
