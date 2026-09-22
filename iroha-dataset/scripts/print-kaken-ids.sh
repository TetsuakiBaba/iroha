#!/bin/sh
# すでに取得済みの KAKEN 課題番号を 1 行 1 件で出す。
# sources.kaken.id_file に渡せば、同じ課題を空振りなしで取り直せる。
#
#   ./scripts/print-kaken-ids.sh > my-award-numbers.txt
set -e
cd "$(dirname "$0")/.."
for f in data/raw/kaken/xml/*.xml; do
  [ -e "$f" ] || { echo "data/raw/kaken/xml/ が空。先に download を実行する" >&2; exit 1; }
  basename "$f" .xml
done
