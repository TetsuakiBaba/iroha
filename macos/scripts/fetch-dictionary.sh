#!/bin/bash
# azooKeyの辞書データ（azooKey_dictionary_storage、Apache-2.0）を vendor/ に取得する。
# 辞書ラティス（AzooKeyKanaKanjiConverter）が読む LOUDS 形式の辞書で、
# make-bundle.sh が iroha.app/Contents/Resources/Dictionary へコピーする。
#
# コミットは Package.swift で指定した AzooKeyKanaKanjiConverter のバージョンが
# サブモジュールとして参照しているものに合わせる（辞書フォーマットの互換性のため）:
#   gh api "repos/azooKey/AzooKeyKanaKanjiConverter/contents/Sources/KanaKanjiConverterModuleWithDefaultDictionary?ref=vX.Y.Z"
set -euo pipefail
# vendor/ はリポジトリルートにある
cd "$(dirname "$0")/../.."

DICT_COMMIT="832fbb0d3039dfaa4b2183956f3d96f6b07eec4d"   # AzooKeyKanaKanjiConverter v0.11.2 が参照
DEST="vendor/azooKey_dictionary_storage"

if [ -f "$DEST/Dictionary/mm.binary" ] && [ "$(git -C "$DEST" rev-parse HEAD 2>/dev/null)" = "$DICT_COMMIT" ]; then
    echo "==> 辞書データは取得済み ($DEST @ ${DICT_COMMIT:0:7})"
    exit 0
fi

echo "==> azooKey辞書データを取得 (${DICT_COMMIT:0:7})"
rm -rf "$DEST"
mkdir -p "$DEST"
git -C "$DEST" init -q
git -C "$DEST" remote add origin https://github.com/azooKey/azooKey_dictionary_storage
git -C "$DEST" fetch -q --depth 1 origin "$DICT_COMMIT"
git -C "$DEST" checkout -q FETCH_HEAD
echo "==> 完了: $DEST/Dictionary ($(du -sh "$DEST/Dictionary" | cut -f1))"
