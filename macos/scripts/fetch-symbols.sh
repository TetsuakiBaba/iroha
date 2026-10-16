#!/bin/bash
# 記号・顔文字・絵文字の候補に使う Mozc のデータ（BSD 3-Clause、Google）を vendor/ に取得する。
# `SymbolCandidateTable`（IrohaCore）が読む TSV で、make-bundle.sh が
# iroha.app/Contents/Resources/Symbols へコピーする。
#
# 取得するのは Mozc の src/data/ のうち Google が書いたデータだけ（README の License 節で
# 第三者のデータとされている dictionary_oss/ は使わない）。
# Unicode の新しい絵文字に合わせるときは MOZC_COMMIT を上げる
set -euo pipefail
# vendor/ はリポジトリルートにある
cd "$(dirname "$0")/../.."

MOZC_COMMIT="4f25784ad1bcab1bbfe7cc2c4a22e32293fd6a48"   # 2026-10-08
DEST="vendor/mozc-data"
FILES=(
    "src/data/symbol/symbol.tsv"
    "src/data/emoticon/emoticon.tsv"
    "src/data/emoticon/categorized.tsv"
    "src/data/emoji/emoji_data.tsv"
    "LICENSE"
)

if [ "$(cat "$DEST/COMMIT" 2>/dev/null)" = "$MOZC_COMMIT" ]; then
    echo "==> Mozc の記号データは取得済み ($DEST @ ${MOZC_COMMIT:0:7})"
    exit 0
fi

echo "==> Mozc の記号データを取得 (${MOZC_COMMIT:0:7})"
rm -rf "$DEST"
mkdir -p "$DEST"
for path in "${FILES[@]}"; do
    curl -sfL --retry 3 -o "$DEST/$(basename "$path")" \
        "https://raw.githubusercontent.com/google/mozc/$MOZC_COMMIT/$path"
done
# emoticon.tsv と categorized.tsv は名前で区別できるので、ディレクトリは平らにする
echo "$MOZC_COMMIT" > "$DEST/COMMIT"
echo "==> 完了: $DEST ($(du -sh "$DEST" | cut -f1))"
