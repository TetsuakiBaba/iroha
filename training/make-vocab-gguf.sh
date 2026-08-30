#!/bin/bash
# 基盤モデルから語彙のみのGGUF（vocab-only）を作る。
#
# 用途: train.py --llama-vocab に渡し、学習データのトークン化を
# llama.cpp（=irohaの推論時）と完全一致させる。
# HF(SentencePiece Unigram Viterbi)とllama.cpp(バイグラム結合近似)は
# 同じ語彙でも分割が食い違うことがあり（AJIMEE-Benchのプロンプトで59%）、
# この不一致は変換精度を大きく下げる（一致82件 acc@1 53.7% vs 不一致118件 33.9%、
# checkpoint-6000での実測）。学習側をllama.cppそのものに合わせて解消する。
#
# 使い方: ./make-vocab-gguf.sh [基盤モデルディレクトリ] [出力gguf]
#   既定: ./base/llm-jp-3-150m → ./llm-jp-3-150m-vocab.gguf
set -euo pipefail
cd "$(dirname "$0")"

BASE="${1:-./base/llm-jp-3-150m}"
OUT="${2:-./$(basename "$BASE")-vocab.gguf}"

PYTHON=python3
[ -x ".venv/bin/python" ] && PYTHON=.venv/bin/python
[ -x "../.venv/bin/python" ] && PYTHON=../.venv/bin/python

# 基盤ディレクトリには書き込まない（学習ジョブと共有されるため）。
# 一時ディレクトリに必要ファイルを集め、特殊トークンを added_tokens.json で追加する。
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
for f in config.json tokenizer.model tokenizer_config.json special_tokens_map.json tokenizer.json; do
    [ -f "$BASE/$f" ] && cp "$BASE/$f" "$TMP/"
done
"$PYTHON" - "$TMP" <<'EOF'
import json, sys
# train.py と同じ特殊トークン・同じID割り当て（語彙末尾に追記）
added = {"\uee00": 99574, "\uee01": 99575, "\uee02": 99576}
with open(f"{sys.argv[1]}/added_tokens.json", "w", encoding="utf-8") as f:
    json.dump(added, f, ensure_ascii=False, indent=2)
EOF

"$PYTHON" ../vendor/llama.cpp/convert_hf_to_gguf.py "$TMP" --vocab-only --outfile "$OUT"
echo "==> 完了: $OUT"
