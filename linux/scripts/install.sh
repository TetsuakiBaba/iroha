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
# インストールのあと「設定 > キーボード > 入力ソース」で「日本語 (iroha)」を足すか、`ibus engine iroha` で切り替える
set -euo pipefail
cd "$(dirname "$0")/../.."

PREFIX="${PREFIX:-/usr/local}"
BUILD_DIR="linux/build"
COMPONENT="$(pkg-config --variable=pkgdatadir ibus-1.0)/component/iroha.xml"

if [ "${1:-}" = "--uninstall" ]; then
    sudo rm -rf "$PREFIX/lib/ibus-iroha"
    sudo rm -f "$COMPONENT"
    ibus write-cache 2>/dev/null || true
    ibus restart
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
pkill -x ibus-engine-iro 2>/dev/null || true
sudo cmake --install "$BUILD_DIR" >/dev/null

echo "==> IBus に読み直させる"
ibus write-cache 2>/dev/null || true
ibus restart

if [ ! -e "${IROHA_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/iroha}/models/iroha-kkc-v1-Q5_K_M.gguf" ]; then
    echo "注意: 変換モデルがまだありません。./linux/scripts/fetch-model.sh で取ってください"
fi
echo "==> 完了（ibus engine iroha で切り替え。記録は ~/.local/share/iroha/logs/ibus-iroha-<ホスト名>.log）"
