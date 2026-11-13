#!/bin/bash
# iroha（Linux版）を入れる。配布物を展開したフォルダで実行する（sudo のパスワードを求められる）。
#   ./install.sh
# 入れる先は files.txt の一覧（/usr/local/lib/iroha・/usr/share/ibus/component/iroha.xml など）。
# 最後に ibus-daemon を起動し直して、入力ソースに iroha を出す
set -euo pipefail
cd "$(dirname "$0")"
# shellcheck source=restart-ibus.sh
. ./restart-ibus.sh

echo "==> 要るライブラリを確かめる"
missing="$(ldd root/usr/local/lib/iroha/* 2>/dev/null | grep 'not found' | sort -u || true)"
if [ -n "$missing" ]; then
    echo "error: 次のライブラリが見つかりません:" >&2
    echo "$missing" >&2
    echo "Ubuntu なら: sudo apt install libibus-1.0-5 libgtk-4-1 libcurl4 libsecret-1-0 libfontconfig1 libharfbuzz0b libxtst6" >&2
    exit 1
fi

echo "==> 入れる（$(wc -l < files.txt) ファイル）"
# 以前の置き場所（2026-11-14 まで）を片付ける
sudo rm -rf /usr/local/lib/ibus-iroha
sudo cp -a --no-preserve=ownership root/. /
sudo install -m 644 files.txt /usr/local/lib/iroha/files.txt
pkill -x -u "$(id -u)" ibus-engine-iro 2>/dev/null || true

echo "==> IBus に読み直させる"
ibus write-cache 2>/dev/null || true
restart_ibus

if [ ! -e "${XDG_DATA_HOME:-$HOME/.local/share}/iroha/models/iroha-kkc-v1-Q5_K_M.gguf" ]; then
    echo "変換モデルを取ります（約82MB。設定画面のモデルのタブからも取れます）"
    ./fetch-model.sh
fi
echo "==> 完了。「設定 > キーボード > 入力ソース」で「日本語 (iroha)」を足してください（ibus engine iroha でも切り替えられます）"
