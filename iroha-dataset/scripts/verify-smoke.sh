#!/bin/sh
# 小規模の動作確認を一通り。変更を入れたあとに走らせる。
#
#   ./scripts/verify-smoke.sh
#
# data-smoke/ に Tatoeba 1000 文 + KAKEN 60 課題ぶんを作り、テストも走らせる。
# KAKEN の取得は 1 件 1 秒空けるので、初回は 2 分ほどかかる（2 回目からはキャッシュ）。
set -e
cd "$(dirname "$0")/.."

PY=./.venv/bin/python
[ -x "$PY" ] || PY=python3

echo "== テスト =="
"$PY" -m pytest tests/ -q

echo
echo "== 小規模ビルド（config/smoke.yaml） =="
"$PY" -m iroha_dataset build-all --config config/smoke.yaml

echo
echo "== 目視確認用 =="
for f in canonical kkc typo; do
  echo "--- data-smoke/samples/${f}_samples.txt（先頭）"
  head -12 "data-smoke/samples/${f}_samples.txt"
  echo
done

echo "レポート: data-smoke/REPORT.md"
