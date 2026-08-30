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
