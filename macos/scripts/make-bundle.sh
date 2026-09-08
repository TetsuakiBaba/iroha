#!/bin/bash
# .build/release/iroha から .build/iroha.app を組み立てて署名する。
# ローカル(install.sh)とCI(release.yml)の両方から使う共通スクリプト。
#
# 環境変数:
#   VERSION       CFBundleShortVersionStringに注入（省略時はInfo.plistのまま）
#   BUILD_NUMBER  CFBundleVersionに注入（省略時はInfo.plistのまま）
#   SIGN_IDENTITY 署名ID（省略時は ad-hoc "-"。CIではDeveloper ID +
#                 hardened runtime + timestampで署名する）
set -euo pipefail
cd "$(dirname "$0")/.."

APP=".build/iroha.app"

echo "==> .appバンドル組み立て"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp .build/release/iroha "$APP/Contents/MacOS/iroha"
cp Resources/Info.plist "$APP/Contents/Info.plist"
cp Resources/main.tiff Resources/en.tiff Resources/AppIcon.icns "$APP/Contents/Resources/"
cp -R Resources/ja.lproj Resources/en.lproj "$APP/Contents/Resources/"
printf 'APPL????' > "$APP/Contents/PkgInfo"

# 辞書ラティス用のazooKey辞書（scripts/fetch-dictionary.sh で vendor/ に取得したもの）。
# LatticeConverter が Contents/Resources/Dictionary から読む。無いと候補ウィンドウが
# zenz単体の挙動に戻るので、組み立て時点で止める
DICT="../vendor/azooKey_dictionary_storage/Dictionary"
if [ ! -f "$DICT/mm.binary" ]; then
  echo "error: 辞書データがありません。./macos/scripts/fetch-dictionary.sh を実行してください" >&2
  exit 1
fi
cp -R "$DICT" "$APP/Contents/Resources/Dictionary"
# SwiftPM依存パッケージのリソースバンドル（Bundle.module の解決先。存在するものだけ）
for bundle in .build/release/*.bundle; do
  [ -d "$bundle" ] && cp -R "$bundle" "$APP/Contents/Resources/"
done

if [ -n "${VERSION:-}" ]; then
  plutil -replace CFBundleShortVersionString -string "$VERSION" "$APP/Contents/Info.plist"
fi
if [ -n "${BUILD_NUMBER:-}" ]; then
  plutil -replace CFBundleVersion -string "$BUILD_NUMBER" "$APP/Contents/Info.plist"
fi

SIGN_IDENTITY="${SIGN_IDENTITY:--}"
if [ "$SIGN_IDENTITY" = "-" ]; then
  echo "==> 署名 (ad-hoc)"
  codesign --force --sign - "$APP"
else
  echo "==> 署名 (Developer ID, hardened runtime)"
  codesign --force --options runtime --timestamp --sign "$SIGN_IDENTITY" "$APP"
fi
