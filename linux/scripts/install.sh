#!/bin/bash
# Linux版をビルドしてこのマシンにインストールし、IBus に読み直させる（sudo が要る）。
#
#   ./linux/scripts/install.sh            # /usr/local/lib/ibus-iroha と /usr/share/ibus/component/iroha.xml
#   PREFIX=/usr ./linux/scripts/install.sh
#   ./linux/scripts/install.sh --uninstall
#
# 入るもの: エンジン（<PREFIX>/lib/ibus-iroha/ibus-engine-iroha）、辞書ラティスの辞書（同 Dictionary/）、
# 記号のデータ（同 Symbols/）、IBus のコンポーネントの定義（<IBus のデータ>/component/iroha.xml）。
# 変換モデルはデータフォルダに置く（linux/scripts/fetch-model.sh）。
# インストールのあと「設定 > キーボード > 入力ソース」で「日本語 (iroha)」を足すか、`ibus engine iroha` で切り替える。
# 変換の学習・入力履歴は <データフォルダ>（~/.local/share/iroha）に残る
set -euo pipefail
cd "$(dirname "$0")/../.."

# ibus-daemon を、動いているものと同じ引数・環境変数で起動し直す（新しいコンポーネントの定義を読ませる）。
# `ibus restart` は、GNOME（Ubuntu 22.04・IBus 1.5.26）が起動した ibus-daemon で使うと、止まったまま戻らなかった
# （2026-11-14。そのあいだデスクトップのアプリで入力できなくなる）
restart_ibus() {
    local pid
    pid="$(pgrep -x -u "$(id -u)" ibus-daemon | head -1 || true)"
    if [ -z "$pid" ]; then
        echo "注意: ibus-daemon が動いていません。ログインし直すと読み込まれます" >&2
        return 0
    fi
    local -a environment arguments
    mapfile -d '' environment < "/proc/$pid/environ"
    mapfile -d '' arguments < "/proc/$pid/cmdline"
    local -a command=()
    local argument
    for argument in "${arguments[@]}"; do
        case "$argument" in
            --replace|-r|--daemonize|-d) ;;
            *) command+=("$argument") ;;
        esac
    done
    (cd / && env -i "${environment[@]}" setsid "${command[@]}" --replace --daemonize)
    local i
    for i in $(seq 1 20); do
        local now
        now="$(pgrep -x -u "$(id -u)" ibus-daemon | head -1 || true)"
        if [ -n "$now" ] && [ "$now" != "$pid" ]; then
            echo "ibus-daemon を起動し直しました（${command[*]}）"
            return 0
        fi
        sleep 0.5
    done
    echo "error: ibus-daemon が起動し直りません。次で起動してください: ${command[*]} --replace --daemonize" >&2
    return 1
}

PREFIX="${PREFIX:-/usr/local}"
BUILD_DIR="linux/build"
COMPONENT="$(pkg-config --variable=pkgdatadir ibus-1.0)/component/iroha.xml"

if [ "${1:-}" = "--uninstall" ]; then
    sudo rm -rf "$PREFIX/lib/ibus-iroha"
    sudo rm -f "$COMPONENT"
    ibus write-cache 2>/dev/null || true
    restart_ibus
    echo "==> 外しました"
    exit 0
fi

for required in vendor/dist-linux/lib/libllama.a vendor/azooKey_dictionary_storage/Dictionary vendor/mozc-data/emoji_data.tsv; do
    if [ ! -e "$required" ]; then
        echo "error: $required がありません（linux/README.md の「ビルドと動作確認」を参照）" >&2
        exit 1
    fi
done

echo "==> ビルド"
cmake -S linux -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
cmake --build "$BUILD_DIR"
"$BUILD_DIR/iroha-core/iroha-core-tests" >/dev/null
"$BUILD_DIR/ibus-iroha/ibus-iroha-tests" >/dev/null

echo "==> インストール（$PREFIX/lib/ibus-iroha・$COMPONENT）"
# 動いている古いエンジンを止める（ibus-daemon が新しい実行ファイルで起動し直す）
pkill -x -u "$(id -u)" ibus-engine-iro 2>/dev/null || true
sudo cmake --install "$BUILD_DIR" >/dev/null

echo "==> IBus に読み直させる"
ibus write-cache 2>/dev/null || true
restart_ibus

if [ ! -e "${IROHA_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/iroha}/models/iroha-kkc-v1-Q5_K_M.gguf" ]; then
    echo "注意: 変換モデルがまだありません。./linux/scripts/fetch-model.sh で取ってください"
fi
echo "==> 完了（ibus engine iroha で切り替え。記録は ~/.local/share/iroha/logs/ibus-iroha-<ホスト名>.log）"
