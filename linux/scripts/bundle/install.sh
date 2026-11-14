#!/bin/bash
# iroha（Linux版）を入れる。配布物を展開したフォルダで実行する（sudo のパスワードを求められる）。
#   ./install.sh              入れて、IBus を起動し直す（ふだんはこれ）
#   ./install.sh --system     管理者の部分だけ（root で実行する。設定画面のアップデートが pkexec で呼ぶ）
#   ./install.sh --session    利用者の部分だけ（エンジンを止めて IBus を起動し直す。アップデートが --system のあとに呼ぶ）
# 入れる先は files.txt の一覧（/usr/local/lib/iroha・/usr/share/ibus/component/iroha.xml など）。
# 最後に ibus-daemon を起動し直して、入力ソースに iroha を出す
set -euo pipefail
cd "$(dirname "$0")"
# shellcheck source=restart-ibus.sh
. ./restart-ibus.sh

check_libraries() {
    local missing
    missing="$(ldd root/usr/local/lib/iroha/* 2>/dev/null | grep 'not found' | sort -u || true)"
    if [ -n "$missing" ]; then
        echo "error: 次のライブラリが見つかりません:" >&2
        echo "$missing" >&2
        echo "Ubuntu なら: sudo apt install libibus-1.0-5 libgtk-4-1 libcurl4 libsecret-1-0 libfontconfig1 libharfbuzz0b libxtst6 libxi6 libxfixes3" >&2
        exit 1
    fi
}

# ファイルを置く（$1 は sudo か空）。
# --remove-destination: 置く前に古いファイルを消す。動いている実行ファイル（設定画面・エンジン）に上書きすると
# 「Text file busy」で止まり、読み込まれている fcitx5 のアドオンに上書きすると読み込んでいるプロセスが落ちるため
# （消したファイルは、使っているプロセスが終わるまで中身が残る）
install_files() {
    local run="$1"
    # 以前の置き場所（2026-11-14 まで）を片付ける
    $run rm -rf /usr/local/lib/ibus-iroha
    $run cp -a --no-preserve=ownership --remove-destination root/. /
    $run install -m 644 files.txt /usr/local/lib/iroha/files.txt
}

# 動いている古いエンジンを止めて、IBus に読み直させる（利用者として実行する）
restart_session() {
    pkill -x -u "$(id -u)" ibus-engine-iro 2>/dev/null || true
    echo "==> IBus に読み直させる"
    ibus write-cache 2>/dev/null || true
    restart_ibus
    start_selection /usr/local/lib/iroha/iroha-selection
    if pgrep -x -u "$(id -u)" fcitx5 >/dev/null 2>&1; then
        echo "注意: fcitx5 を使っている場合は、fcitx5 を起動し直すと新しい版になります" >&2
    fi
}

case "${1:-}" in
    --system)
        if [ "$(id -u)" -ne 0 ]; then
            echo "error: --system は root で実行してください" >&2
            exit 1
        fi
        check_libraries
        echo "==> 入れる（$(wc -l < files.txt) ファイル）"
        install_files ""
        exit 0
        ;;
    --session)
        restart_session
        exit 0
        ;;
    "") ;;
    *)
        echo "使い方: $0 [--system | --session]" >&2
        exit 2
        ;;
esac

echo "==> 要るライブラリを確かめる"
check_libraries
echo "==> 入れる（$(wc -l < files.txt) ファイル）"
install_files sudo
restart_session

if [ ! -e "${XDG_DATA_HOME:-$HOME/.local/share}/iroha/models/iroha-kkc-v1-Q5_K_M.gguf" ]; then
    echo "変換モデルを取ります（約82MB。設定画面のモデルのタブからも取れます）"
    ./fetch-model.sh
fi
echo "==> 完了。「設定 > キーボード > 入力ソース」で「日本語 (iroha)」を足してください（ibus engine iroha でも切り替えられます）"
