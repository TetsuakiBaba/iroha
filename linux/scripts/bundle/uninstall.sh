#!/bin/bash
# iroha（Linux版）を外す。install.sh が入れたファイル（/usr/local/lib/iroha/files.txt の一覧）を消す。
# データフォルダ（~/.local/share/iroha。学習・ユーザ辞書・モデル）は消さない
set -euo pipefail
cd "$(dirname "$0")"
# shellcheck source=restart-ibus.sh
. ./restart-ibus.sh

LIST=/usr/local/lib/iroha/files.txt
[ -f "$LIST" ] || LIST=files.txt
pkill -x -u "$(id -u)" ibus-engine-iro 2>/dev/null || true
pkill -x -u "$(id -u)" iroha-selection 2>/dev/null || true
while IFS= read -r path; do
    case "$path" in /*) sudo rm -f "$path" ;; esac
done < "$LIST"
sudo rm -rf /usr/local/lib/iroha
ibus write-cache 2>/dev/null || true
restart_ibus
echo "==> 外しました（データフォルダ ~/.local/share/iroha は残しています）"
