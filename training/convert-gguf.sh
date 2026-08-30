#!/bin/bash
# 学習済みモデル（HuggingFace形式）をGGUFに変換し、irohaのモデルフォルダに配置する。
#
# 使い方: ./convert-gguf.sh ./iroha-llmjp-150m
#
# 対応アーキテクチャ:
# - Llama系（llm-jp-3等・SentencePiece）: モデルディレクトリに tokenizer.model が必要
#   （train.py が基盤モデルから自動コピーする。無い場合はREADME参照）
# - GPT-2系（ku-nlp/gpt2-small-japanese-char等・BPE）: convert_hf_to_gguf.py が
#   pre-tokenizerを特定できず止まることがある。その場合は生成後に
#   tokenizer.ggml.pre を 'gpt2-small-japanese-char' に書き換えれば、
#   iroha同梱のllama.cpp（パッチ適用済み）で動く。
set -euo pipefail
cd "$(dirname "$0")/.."

MODEL_DIR="${1:?使い方: ./convert-gguf.sh <HFモデルのディレクトリ>}"
NAME=$(basename "$MODEL_DIR")
OUT_DIR="$HOME/Library/Application Support/iroha/models"
OUT_FILE="$OUT_DIR/$NAME-f16.gguf"

# 変換（要: pip install -r vendor/llama.cpp/requirements.txt 相当。
# training/.venv があればそれを使う）
PYTHON=python3
[ -x "training/.venv/bin/python" ] && PYTHON=training/.venv/bin/python
[ -x ".venv/bin/python" ] && PYTHON=.venv/bin/python
"$PYTHON" vendor/llama.cpp/convert_hf_to_gguf.py "$MODEL_DIR" \
    --outfile "$OUT_FILE" --outtype f16

# トークナイザ種別を表示（BPE系のときだけ pre-tokenizer 名を確認）
"$PYTHON" - "$OUT_FILE" <<'EOF'
import sys
sys.path.insert(0, "vendor/llama.cpp/gguf-py")
import gguf
reader = gguf.GGUFReader(sys.argv[1], "r")

def read_str(name):
    field = reader.get_field(name)
    if field is None:
        return None
    return bytes(field.parts[field.data[0]]).decode()

model = read_str("tokenizer.ggml.model")
print("tokenizer.ggml.model =", model)
if model == "gpt2":  # BPE系のみ pre-tokenizer 名が実行時に照合される
    pre = read_str("tokenizer.ggml.pre")
    print("tokenizer.ggml.pre =", pre)
    if pre not in ("gpt2-small-japanese-char", "gpt-2"):
        print("警告: pre-tokenizer名がiroha同梱llama.cppに未登録の可能性があります。")
        print("       patches/llama-cpp-zenz-pretokenizer.patch を参照してください。")
else:
    print("SentencePiece/Unigram系のため pre-tokenizer の追加登録は不要です。")
EOF

echo "==> 完了: $OUT_FILE"
echo "irohaの設定ウィンドウでこのファイルをモデルパスに指定し、irohaを再起動してください。"
echo "動作確認: IROHA_MODEL=\"$OUT_FILE\" .build/debug/iroha-cli bench testdata/eval.tsv"
