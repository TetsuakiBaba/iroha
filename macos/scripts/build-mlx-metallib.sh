#!/bin/bash
# mlx-swift（iroha-train の追加学習用）の Metal カーネルをコンパイルして metallib を作る。
#
# mlx-swift は SwiftPM の `swift build` では .metal をビルドできない（Xcode のビルドシステム
# だけが対応）。MLX 本体はカーネルの大半を実行時に JIT コンパイルするが、一部
# （arg_reduce / conv / gemv / layer_norm / random / rms_norm / rope / sdpa / steel_attention）は
# 事前コンパイル済みの metallib を必要とする。ここでは mlx-swift の checkout にある
# `Source/Cmlx/mlx-generated/metal/` の .metal を `xcrun metal` で自前コンパイルし、MLX が
# 探す場所のひとつ「SwiftPM リソースバンドル `mlx-swift_Cmlx.bundle/default.metallib`」に置く。
# バンドルは .build/<config>/ に作られ、make-bundle.sh が他の *.bundle と同様に
# Contents/Resources へコピーするので、追加の配置処理は要らない。
#
# 前提: Metal Toolchain（Xcode 26 では別コンポーネント）。無ければ
#   xcodebuild -downloadComponent MetalToolchain
#
# 使い方（macos/ から。install.sh と release.yml が呼ぶ）:
#   ./scripts/build-mlx-metallib.sh [debug|release]   （既定 release）
set -euo pipefail
cd "$(dirname "$0")/.."

CONFIG="${1:-release}"
CHECKOUT=".build/checkouts/mlx-swift"
GEN="$CHECKOUT/Source/Cmlx/mlx-generated/metal"
BIN_DIR=$(swift build -c "$CONFIG" --show-bin-path)
BUNDLE="$BIN_DIR/mlx-swift_Cmlx.bundle"
OUT="$BUNDLE/default.metallib"

if [ ! -d "$GEN" ]; then
  echo "error: mlx-swift の checkout がありません（先に swift build --product iroha-train を実行）: $GEN" >&2
  exit 1
fi
if ! xcrun -sdk macosx metal --version >/dev/null 2>&1; then
  echo "error: Metal Toolchain がありません。xcodebuild -downloadComponent MetalToolchain を実行してください" >&2
  exit 1
fi

# checkout（= mlx-swift のバージョン）より新しければ作り直さない
if [ -f "$OUT" ] && [ -z "$(find "$GEN" -newer "$OUT" -name '*.metal' -o -newer "$OUT" -name '*.h' | head -1)" ]; then
  echo "==> mlx.metallib は最新: $OUT"
  exit 0
fi

echo "==> Metal カーネルをコンパイル ($CONFIG)"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# mlx 本体の kernels/CMakeLists.txt と同じフラグ（-fno-fast-math が数値一致に効く）
FLAGS=(-x metal -Wall -Wextra -fno-fast-math -Wno-c++17-extensions -Wno-c++20-extensions
       -mmacosx-version-min=14.0 "-I$GEN")
AIRS=()
while IFS= read -r SRC; do
  NAME=$(echo "${SRC#"$GEN/"}" | tr '/' '_')
  AIR="$WORK/${NAME%.metal}.air"
  echo "    ${SRC#"$GEN/"}"
  xcrun -sdk macosx metal "${FLAGS[@]}" -c "$SRC" -o "$AIR"
  AIRS+=("$AIR")
done < <(find "$GEN" -name '*.metal' | sort)

mkdir -p "$BUNDLE"
xcrun -sdk macosx metallib "${AIRS[@]}" -o "$OUT.tmp"
mv "$OUT.tmp" "$OUT"
echo "==> 生成: $OUT ($(du -h "$OUT" | cut -f1))"
