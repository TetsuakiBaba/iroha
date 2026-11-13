#!/bin/bash
# install.sh・配布物の install.sh / uninstall.sh が読み込む（source）関数。
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

