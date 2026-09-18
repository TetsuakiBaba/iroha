#!/bin/bash
# irohaをビルドして ~/Library/Input Methods/ にインストールする。
# 初回のみ: システム設定 > キーボード > 入力ソース > 編集 > + から
# 「日本語」→「iroha」を追加する（表示されない場合は一度ログアウト）。
set -euo pipefail
cd "$(dirname "$0")/.."

echo "==> ビルド"
swift build -c release --product iroha
# 追加学習ヘルパー（MLX）。Metal カーネルは swift build では作れないので専用スクリプトで生成する
swift build -c release --product iroha-train
./scripts/build-mlx-metallib.sh release
./scripts/fetch-dictionary.sh

APP=".build/iroha.app"
DEST="$HOME/Library/Input Methods/iroha.app"

# バンドル組み立てと署名（SIGN_IDENTITY未指定ならad-hoc）。
# 開発ビルドはgit describeのバージョン（例: 0.4.2-3-g7aae579）を表示して
# リリースビルドと区別できるようにする（リリースはCIがタグから注入）
VERSION="${VERSION:-$(git describe --tags --dirty --always 2>/dev/null | sed 's/^v//')}" \
  ./scripts/make-bundle.sh

echo "==> インストール: $DEST"
mkdir -p "$HOME/Library/Input Methods"
# rm -rf → cp -R だと、コピーが終わるまでの約1.2秒バンドルが存在しない。その間システムは
# irohaを入力ソース一覧から外し（選択はABCに落ちる）、戻しても メニューバーの入力メニューは
# 畳まれた表示のまま残ることがある。同じ場所に「一度も欠けない」よう、隣に組み立ててから
# rename で入れ替える（renameは瞬時なので一覧から消えない）
STAGING="$HOME/Library/Input Methods/.iroha-staging.app"
OLD="$HOME/Library/Input Methods/.iroha-old-$$.app"
rm -rf "$STAGING" "$OLD"
cp -R "$APP" "$STAGING"
if [ -d "$DEST" ]; then
    mv "$DEST" "$OLD"
    mv "$STAGING" "$DEST" || { mv "$OLD" "$DEST"; exit 1; }
    rm -rf "$OLD"
else
    mv "$STAGING" "$DEST"
fi

# 旧プロセスを終了（新しいバンドルは別ファイルなので、生かしたままだと旧バイナリが動き続ける）
pkill -f "Input Methods/iroha.app/Contents/MacOS/iroha" 2>/dev/null || true

echo "==> 入力ソース登録"
swift scripts/register-input-source.swift || true

# 終了したまま放置すると、入力ソースの選択はirohaのままなのにプロセスが居ない状態になる。
# システムは最初のキー入力で起動し直すが、起動が終わるまでの数打鍵は変換されず
# 英字のまま入る（実測: aiueo → aiuえお）。これがデバッグ中に「入力ソースを
# 選び直さないと直らない」ように見える正体なので、ここで先に起動しておく
# （配布版のアップデータ・セルフインストーラも AppRestarter で同じことをしている）
echo "==> 新しいirohaを起動"
open "$DEST"
for _ in $(seq 25); do
    pgrep -f "Input Methods/iroha.app/Contents/MacOS/iroha" >/dev/null && break
    sleep 0.2
done

echo "==> 完了"
echo "ログ確認: log stream --predicate 'process == \"iroha\"' --style compact"
