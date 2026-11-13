#!/bin/bash
# Linux版の配布物 dist/iroha-<版>-linux-x86_64.tar.gz を作る（GitHub Release に添付するもの）。
#
#   ./linux/scripts/make-bundle.sh [版]     # 版を省くと git describe から（先頭の v は除く）
#
# 中身: install.sh・uninstall.sh・fetch-model.sh・README.txt・files.txt（入れるファイルの一覧）と、
# 入れる先の形のまま並べた root/（usr/local/lib/iroha・usr/share/ibus/component/iroha.xml・
# usr/local/share/applications・etc/xdg/autostart・fcitx5 の開発用ファイルがあれば fcitx5 のアドオン）。
# アセット名は .tar.gz にする（macOS版の UpdateChecker は「iroha- で始まり .zip で終わる」最初のアセットを取るので、
# 名前が重ならないように）
set -euo pipefail
cd "$(dirname "$0")/../.."

VERSION="${1:-$(git describe --tags --always --dirty | sed 's/^v//')}"
NAME="iroha-$VERSION-linux-x86_64"
BUILD_DIR="${IROHA_BUNDLE_BUILD_DIR:-$HOME/.cache/iroha/bundle-build}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/$NAME"

for required in vendor/dist-linux/lib/libllama.a vendor/azooKey_dictionary_storage/Dictionary vendor/mozc-data/emoji_data.tsv; do
    if [ ! -e "$required" ]; then
        echo "error: $required がありません（linux/README.md の「ビルドと動作確認」を参照）" >&2
        exit 1
    fi
done

echo "==> ビルド（$BUILD_DIR）"
cmake -S linux -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local >/dev/null
cmake --build "$BUILD_DIR"
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "==> 並べる"
mkdir -p "$STAGE/root"
DESTDIR="$STAGE/root" cmake --install "$BUILD_DIR" >/dev/null
cp linux/scripts/bundle/install.sh linux/scripts/bundle/uninstall.sh linux/scripts/bundle/README.txt \
   linux/scripts/restart-ibus.sh linux/scripts/fetch-model.sh "$STAGE/"
cp LICENSE "$STAGE/LICENSE" 2>/dev/null || true
(cd "$STAGE/root" && find . -type f -o -type l | sed 's#^\./#/#' | sort) > "$STAGE/files.txt"
echo "$VERSION" > "$STAGE/VERSION"

mkdir -p dist
tar -C "$WORK" -czf "dist/$NAME.tar.gz" "$NAME"
echo "==> 完了: dist/$NAME.tar.gz（$(du -h "dist/$NAME.tar.gz" | cut -f1)・$(wc -l < "$STAGE/files.txt") ファイル）"
