#!/bin/bash
# irohaをビルドして ~/Library/Input Methods/ にインストールする。
# 初回のみ: システム設定 > キーボード > 入力ソース > 編集 > + から
# 「日本語」→「iroha」を追加する（表示されない場合は一度ログアウト）。
set -euo pipefail
cd "$(dirname "$0")/.."

echo "==> ビルド"
swift build -c release --product iroha

APP=".build/iroha.app"
DEST="$HOME/Library/Input Methods/iroha.app"

# バンドル組み立てと署名（SIGN_IDENTITY未指定ならad-hoc）。
# 開発ビルドはgit describeのバージョン（例: 0.4.2-3-g7aae579）を表示して
# リリースビルドと区別できるようにする（リリースはCIがタグから注入）
VERSION="${VERSION:-$(git describe --tags --dirty --always 2>/dev/null | sed 's/^v//')}" \
  ./scripts/make-bundle.sh

echo "==> インストール: $DEST"
mkdir -p "$HOME/Library/Input Methods"
rm -rf "$DEST"
cp -R "$APP" "$DEST"

# 旧プロセスを終了（次回入力時にシステムが新しいバイナリを起動する）
pkill -f "Input Methods/iroha.app/Contents/MacOS/iroha" 2>/dev/null || true

echo "==> 入力ソース登録"
swift scripts/register-input-source.swift || true

echo "==> 完了"
echo "ログ確認: log stream --predicate 'process == \"iroha\"' --style compact"
