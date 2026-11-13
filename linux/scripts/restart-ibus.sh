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


# 選択テキストのAI編集（iroha-selection）を、デスクトップのセッションの環境変数で起動する（ログインを待たずに使えるように。
# ふだんは /etc/xdg/autostart から起動する）。入れ直すたびに前のものを止めてから起動する。
# 環境変数は gnome-shell（無ければ ibus-daemon）のものを使う（このスクリプトを動かした端末のものを使うと、
# エディタの端末などの GTK の設定が混ざる）
start_selection() {
    local program="$1"
    pkill -x -u "$(id -u)" iroha-selection 2>/dev/null || true
    [ -x "$program" ] || return 0
    local pid
    pid="$(pgrep -x -u "$(id -u)" gnome-shell | head -1 || true)"
    [ -n "$pid" ] || pid="$(pgrep -x -u "$(id -u)" ibus-daemon | head -1 || true)"
    if [ -z "$pid" ]; then
        echo "注意: デスクトップのセッションが見つかりません。選択テキストのAI編集は次のログインから動きます" >&2
        return 0
    fi
    local -a environment
    mapfile -d '' environment < "/proc/$pid/environ"
    if ! printf '%s\n' "${environment[@]}" | grep -q '^DISPLAY='; then
        echo "注意: X11 のセッションではないため、選択テキストのAI編集は動きません" >&2
        return 0
    fi
    (cd / && env -i "${environment[@]}" setsid "$program" </dev/null >/dev/null 2>&1 &)
    echo "選択テキストのAI編集（iroha-selection）を起動しました"
}
