#!/bin/bash
# .build/release/iroha から .build/iroha.app を組み立てて署名する。
# ローカル(install.sh)とCI(release.yml)の両方から使う共通スクリプト。
#
# 環境変数:
#   VERSION       CFBundleShortVersionStringに注入（省略時はInfo.plistのまま）
#   BUILD_NUMBER  CFBundleVersionに注入（省略時はInfo.plistのまま）
#   SIGN_IDENTITY 署名ID（省略時は ad-hoc "-"。CIではDeveloper ID +
#                 hardened runtime + timestampで署名する）
#   SIGN_TIMESTAMP 0 なら署名に時刻を付けない（Apple のサーバに接続しない。公証しないローカル用。既定 1）
set -euo pipefail
cd "$(dirname "$0")/.."

APP=".build/iroha.app"

echo "==> .appバンドル組み立て"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp .build/release/iroha "$APP/Contents/MacOS/iroha"
# 追加学習ヘルパー（設定画面が Process で起動する）。MLX の Metal カーネルは
# .build/release/mlx-swift_Cmlx.bundle/default.metallib にあり、下の *.bundle コピーで Resources に入る
if [ ! -f .build/release/iroha-train ] || [ ! -f .build/release/mlx-swift_Cmlx.bundle/default.metallib ]; then
  echo "error: iroha-train か mlx.metallib がありません。swift build -c release --product iroha-train と" >&2
  echo "       ./scripts/build-mlx-metallib.sh release を実行してください" >&2
  exit 1
fi
cp .build/release/iroha-train "$APP/Contents/MacOS/iroha-train"
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

# 記号・顔文字・絵文字の候補に使う Mozc のデータ（scripts/fetch-symbols.sh で vendor/ に取得したもの、BSD 3-Clause）。
# SymbolCandidateTable が Contents/Resources/Symbols から読む。BSD の条件で LICENSE も一緒に置く
SYMBOLS="../vendor/mozc-data"
for file in symbol.tsv emoticon.tsv categorized.tsv emoji_data.tsv LICENSE; do
  if [ ! -f "$SYMBOLS/$file" ]; then
    echo "error: 記号のデータがありません。./macos/scripts/fetch-symbols.sh を実行してください" >&2
    exit 1
  fi
done
mkdir -p "$APP/Contents/Resources/Symbols"
cp "$SYMBOLS"/{symbol.tsv,emoticon.tsv,categorized.tsv,emoji_data.tsv,LICENSE} "$APP/Contents/Resources/Symbols/"

# 打ち間違い訂正モデルは**同梱しない**。設定でONにしたときに
# TypoNormalizerDownloader が models/typo-normalizer.json 経由で取得する
# （重みは本体コード(MIT)と別ライセンス(CC BY-SA 4.0)なので配布物を分けてある）。
# 公開の手順は ./macos/scripts/publish-typo-normalizer.sh
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

# ネストした実行ファイル（iroha-train）は外側の署名に含まれないので先に個別に署名する
SIGN_IDENTITY="${SIGN_IDENTITY:--}"
if [ "$SIGN_IDENTITY" = "-" ]; then
  echo "==> 署名 (ad-hoc)"
  codesign --force --sign - "$APP/Contents/MacOS/iroha-train"
  codesign --force --sign - "$APP"
else
  echo "==> 署名 (Developer ID, hardened runtime)"
  TIMESTAMP="--timestamp"
  [ "${SIGN_TIMESTAMP:-1}" = "0" ] && TIMESTAMP="--timestamp=none"
  codesign --force --options runtime "$TIMESTAMP" --sign "$SIGN_IDENTITY" "$APP/Contents/MacOS/iroha-train"
  codesign --force --options runtime "$TIMESTAMP" --sign "$SIGN_IDENTITY" "$APP"
fi
