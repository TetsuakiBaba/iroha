#!/bin/bash
# 打ち間違い訂正モデル（training/typo-normalizer の export_model.py の書き出し）を
# 配布用に整えて、GitHub Releases の専用タグへ公開する。
#
#   ./macos/scripts/publish-typo-normalizer.sh <書き出しディレクトリ> <モデルID> [--float32 | --q6]
#   例: ./macos/scripts/publish-typo-normalizer.sh export-scale-16x small-v1
#
#   --q6（2026-11-11、iroha-typo-v3 から）: 線形層の重みを 6 ビットに落とす（float16 の約 4 割）。量子化した重みは
#   float32 の期待値とは数件ずれるので、照合には Q6_PARITY=<parity.json> で「同じ規則で量子化して戻した PyTorch の
#   モデル」の期待値を渡す（training/typo-normalizer の quant_sim.py → export_model.py が作る）。完全に一致するはず。
#   q6 の重みはアプリの v0.14.5 以降でないと読めないので、カタログは models/typo-normalizer-format2.json に載せる
#
#   LICENSE の「学習元」段落は既定で zenz-v2.5-dataset（small-v1 の学習元）。学習元の違うモデルは
#   TRAINED_ON=<段落を書いたテキストファイル> で差し替える（ライセンスはモデルごとに変わりうる）
#
# やること:
#   1. 重みを float16 に落とす（既定。12.8MB → 6.4MB）
#   2. 移植の照合（parity 200件）を回して、配るものが正しいことを確かめる
#   3. LICENSE / README を添えて dist/typo-normalizer/<ID>/ に置く
#   4. カタログ（models/typo-normalizer-format2.json。q6 でなければ形式 1 の models/typo-normalizer.json にも載せられる）に入れる内容を表示する
#   5. アップロードのコマンドを表示する（**実行はしない**）
#
# 配布の考えかた:
#   - 重みは本体コード(MIT)と別ライセンス CC BY-SA 4.0。学習元 zenz-v2.5-dataset が
#     CC BY-SA 4.0（一部 ODC-BY / Common Crawl 規約）なので、継承して同じ条件で配る
#   - アプリのリリース（vX.Y.Z）とは別タグにする。モデルの更新頻度が違うし、
#     毎リリース 6.4MB を載せ直すと URL も変わってしまうため
#   - **必ずプレリリースとして作る**。通常リリースにすると GitHub の releases/latest が
#     これを指してしまい、UpdateChecker（アプリの更新通知）が壊れる
set -euo pipefail
cd "$(dirname "$0")/../.."   # リポジトリのルート

SOURCE="${1:-}"
MODEL_ID="${2:-}"
PRECISION="${3:-}"
TAG="typo-normalizer-v1"
REPO="TetsuakiBaba/iroha"

if [ -z "$SOURCE" ] || [ -z "$MODEL_ID" ]; then
  echo "使い方: ./macos/scripts/publish-typo-normalizer.sh <書き出しディレクトリ> <モデルID> [--float32]" >&2
  exit 1
fi
if [ ! -f "$SOURCE/manifest.json" ] || [ ! -f "$SOURCE/weights.bin" ]; then
  echo "error: $SOURCE に manifest.json / weights.bin がありません" >&2
  exit 1
fi

DEST="dist/typo-normalizer/$MODEL_ID"
CLI="macos/.build/release/iroha-cli"
if [ ! -x "$CLI" ]; then
  echo "==> iroha-cli をビルド（float16 変換と照合に使う）"
  (cd macos && swift build --build-system native -c release --product iroha-cli)
fi

rm -rf "$DEST"
mkdir -p "$DEST"
SOURCE_DTYPE=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["dtype"])'   "$SOURCE/manifest.json")
if [ "$PRECISION" = "--q6" ]; then
  if [ -z "${Q6_PARITY:-}" ] || [ ! -f "$Q6_PARITY" ]; then
    echo "error: --q6 には Q6_PARITY=<量子化して戻したモデルの parity.json> が要ります" >&2
    exit 1
  fi
  echo "==> 線形層を q6 に落とす"
  IROHA_TYPO_MODEL="$SOURCE" "$CLI" typo shrink "$DEST" --q6 >/dev/null
elif [ "$PRECISION" = "--float32" ]; then
  echo "==> float32 のまま用意"
  cp "$SOURCE/manifest.json" "$SOURCE/weights.bin" "$DEST/"
elif [ "$SOURCE_DTYPE" = "float16-le" ]; then
  echo "==> 既に float16 なのでそのままコピー"
  cp "$SOURCE/manifest.json" "$SOURCE/weights.bin" "$DEST/"
else
  echo "==> float16 に落とす"
  IROHA_TYPO_MODEL="$SOURCE" "$CLI" typo shrink "$DEST" >/dev/null
fi

echo "==> 移植の照合（PyTorch実装と同じ数を出すか）"
if [ "$PRECISION" = "--q6" ]; then
  cp "$Q6_PARITY" "$DEST/parity.json"
  "$CLI" typo parity --dir "$DEST"
  rm "$DEST/parity.json"
elif [ -f "$SOURCE/parity.json" ]; then
  cp "$SOURCE/parity.json" "$DEST/parity.json"
  "$CLI" typo parity --dir "$DEST"
  rm "$DEST/parity.json"   # 照合用なので配らない
else
  echo "warning: parity.json が無いので照合を飛ばします" >&2
fi

if [ -n "${TRAINED_ON:-}" ]; then
  TRAINED_ON_TEXT=$(cat "$TRAINED_ON")
else
  TRAINED_ON_TEXT='The model was trained on readings (kana) derived from:
  zenz-v2.5-dataset by Keita Miwa
  https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset
  Licensed under CC BY-SA 4.0. The subset derived from llm-jp-corpus-v3 is
  covered by ODC-BY and the Common Crawl terms of use.'
fi
cat > "$DEST/LICENSE" <<LICENSE_EOF
iroha typo normalizer model weights
Copyright (c) 2026 Tetsuaki Baba

These model weights are licensed under the Creative Commons
Attribution-ShareAlike 4.0 International License (CC BY-SA 4.0).
https://creativecommons.org/licenses/by-sa/4.0/

$TRAINED_ON_TEXT

Note: the iroha application itself is MIT licensed. Only these weights are
CC BY-SA 4.0, which is why they are distributed separately from the app.
LICENSE_EOF

cat > "$DEST/README.md" <<README_EOF
# iroha typo normalizer — $MODEL_ID

ローマ字入力の打ち間違いを、かな漢字変換の手前で直す「読み → 読み」のモデル。
文字単位 Transformer encoder–decoder。iroha が設定でONにしたときに取得して使う。

- 学習・評価: \`training/typo-normalizer/\`（リポジトリには含めていない）
- 推論の実装: 同 \`macos/Sources/IrohaCore/TypoNormalizer/\`
- ライセンス: CC BY-SA 4.0（LICENSE を参照）

\`manifest.json\` にモデル構成と語彙、\`weights.bin\` に全テンソルを
little-endian で連結したものが入っている（\`dtype\` は manifest を参照）。
README_EOF

echo
echo "==> 用意できました: $DEST"
MANIFEST_BYTES=$(stat -f%z "$DEST/manifest.json")
WEIGHTS_BYTES=$(stat -f%z "$DEST/weights.bin")
MANIFEST_SHA=$(shasum -a 256 "$DEST/manifest.json" | cut -d' ' -f1)
WEIGHTS_SHA=$(shasum -a 256 "$DEST/weights.bin" | cut -d' ' -f1)
BASE="https://github.com/$REPO/releases/download/$TAG"

echo
echo "models/typo-normalizer-format2.json に入れる内容:"
cat <<JSON_EOF
    {
      "id": "$MODEL_ID",
      "name": "（設定画面に出す名前）",
      "summary": "（パラメータ数・速度など）",
      "manifest": {
        "url": "$BASE/$MODEL_ID-manifest.json",
        "bytes": $MANIFEST_BYTES,
        "sha256": "$MANIFEST_SHA"
      },
      "weights": {
        "url": "$BASE/$MODEL_ID-weights.bin",
        "bytes": $WEIGHTS_BYTES,
        "sha256": "$WEIGHTS_SHA"
      }
    }
JSON_EOF

echo
# アップロード用にモデルIDを前置きした名前で複製する。
# gh の `file#label` は表示ラベルだけを変えるもので、ダウンロード名（= URL）は
# ファイル名のままになる。モデルを増やしたときに名前が衝突しないよう、実ファイル名を分ける
mkdir -p "$DEST/upload"
for f in manifest.json weights.bin LICENSE README.md; do
  cp "$DEST/$f" "$DEST/upload/$MODEL_ID-$f"
done

echo "アップロード（内容を確認してから手で実行してください）:"
echo "  gh release create $TAG --repo $REPO --prerelease \\"
echo "    --title 'Typo normalizer models' \\"
echo "    --notes 'iroha の打ち間違い訂正モデル。CC BY-SA 4.0。アプリが設定からダウンロードします' || true"
echo "  gh release upload $TAG --repo $REPO --clobber $DEST/upload/*"
echo
echo "そのあと models/typo-normalizer-format2.json を更新して push すると、アプリから見えるようになります。"
echo "※ --prerelease を外さないこと（外すと releases/latest がこれを指し、更新通知が壊れます）"
