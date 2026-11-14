#!/bin/bash
# Linux版をビルドしてこのマシンにインストールし、IBus に読み直させる（sudo が要る）。
#
#   ./linux/scripts/install.sh            # /usr/local/lib/iroha と /usr/share/ibus/component/iroha.xml
#   PREFIX=/usr ./linux/scripts/install.sh
#   ./linux/scripts/install.sh --uninstall
#
# 入るもの: <PREFIX>/lib/iroha に IBus のエンジン（ibus-engine-iroha）・設定画面（iroha-settings）・
# 選択テキストのAI編集（iroha-selection）・変換のベンチマークの計測（iroha-bench）・辞書ラティスの辞書（Dictionary/）・
# 記号のデータ（Symbols/）、
# IBus のコンポーネントの定義（<IBus のデータ>/component/iroha.xml）、設定画面の .desktop、
# 選択テキストのAI編集の自動起動（/etc/xdg/autostart）、fcitx5 の開発用ファイルがあれば fcitx5 のアドオン（/usr の下）。
# 変換モデルはデータフォルダに置く（linux/scripts/fetch-model.sh）。
# インストールのあと「設定 > キーボード > 入力ソース」で「日本語 (iroha)」を足すか、`ibus engine iroha` で切り替える。
# 変換の学習・入力履歴は <データフォルダ>（~/.local/share/iroha）に残る
set -euo pipefail
cd "$(dirname "$0")/../.."

# shellcheck source=restart-ibus.sh
. "$(dirname "$0")/restart-ibus.sh"

PREFIX="${PREFIX:-/usr/local}"
BUILD_DIR="linux/build"
COMPONENT="$(pkg-config --variable=pkgdatadir ibus-1.0)/component/iroha.xml"

if [ "${1:-}" = "--uninstall" ]; then
    pkill -x -u "$(id -u)" iroha-selection 2>/dev/null || true
    if [ -f "$BUILD_DIR/install_manifest.txt" ]; then
        xargs -d '\n' sudo rm -f < "$BUILD_DIR/install_manifest.txt"
    fi
    sudo rm -rf "$PREFIX/lib/iroha" "$PREFIX/lib/ibus-iroha"
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
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "==> インストール（$PREFIX/lib/iroha・$COMPONENT）"
# 以前の置き場所（2026-11-14 まで。IBus 専用の名前だった）を片付ける
sudo rm -rf "$PREFIX/lib/ibus-iroha"
# 動いている古いエンジンを止める（ibus-daemon が新しい実行ファイルで起動し直す）
pkill -x -u "$(id -u)" ibus-engine-iro 2>/dev/null || true
pkill -x -u "$(id -u)" iroha-selection 2>/dev/null || true
sudo cmake --install "$BUILD_DIR" >/dev/null

echo "==> IBus に読み直させる"
ibus write-cache 2>/dev/null || true
restart_ibus

if [ ! -e "${IROHA_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/iroha}/models/iroha-kkc-v1-Q5_K_M.gguf" ]; then
    echo "注意: 変換モデルがまだありません。./linux/scripts/fetch-model.sh で取ってください"
fi
start_selection "$PREFIX/lib/iroha/iroha-selection"
echo "==> 完了（ibus engine iroha で切り替え。記録は ~/.local/share/iroha/logs/ibus-iroha-<ホスト名>.log）"
