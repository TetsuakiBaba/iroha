#!/bin/bash
# llama.cpp を静的ライブラリとしてビルドし、vendor/dist-linux に置く（Linux 用。CPU 推論）。
#
# - ソースはリポジトリ直下の vendor/llama.cpp を macOS版・Windows版と共有する（zenz 用のパッチが要る）
# - vendor/dist（macOS 用）・vendor/dist-windows（Windows 用）には触らない
# - 中間ファイルは Dropbox の外（~/.cache/iroha/llama-build-linux）に作る。vendor/llama.cpp/build は
#   macOS版のビルド先で、Dropbox で同期されているため
# - 配る実行ファイルが動く CPU を広くするため、手元の CPU 向けの最適化（GGML_NATIVE）は使わず AVX2 までにする
#   （Windows版の build-llama.ps1 と同じ）
#
# 前提: cmake と ninja（例: python3 -m venv ~/venvs/iroha-build && ~/venvs/iroha-build/bin/pip install cmake ninja
#       として PATH に足す。apt の cmake / ninja-build でもよい）
set -euo pipefail
cd "$(dirname "$0")/../.."

LLAMA_TAG="b10689"   # macos/scripts/build-llama.sh と合わせる
LLAMA_DIR="vendor/llama.cpp"
BUILD_DIR="${IROHA_LLAMA_BUILD_DIR:-$HOME/.cache/iroha/llama-build-linux}"
DIST_DIR="vendor/dist-linux"

if [ ! -d "$LLAMA_DIR" ]; then
    echo "==> llama.cpp ($LLAMA_TAG) を取得"
    git clone --depth 1 --branch "$LLAMA_TAG" https://github.com/ggml-org/llama.cpp "$LLAMA_DIR"
fi

# zenz の pre-tokenizer 名を認識させるパッチ（適用済みなら飛ばす）
if ! grep -q "gpt2-small-japanese-char" "$LLAMA_DIR/src/llama-vocab.cpp"; then
    echo "==> zenz 対応パッチを適用"
    git -C "$LLAMA_DIR" apply ../../patches/llama-cpp-zenz-pretokenizer.patch
fi

echo "==> CMake configure（$BUILD_DIR）"
cmake -S "$LLAMA_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_OPENMP=OFF \
    -DGGML_NATIVE=OFF \
    -DGGML_AVX=ON \
    -DGGML_AVX2=ON \
    -DGGML_FMA=ON \
    -DGGML_F16C=ON \
    -DGGML_BMI2=ON \
    -DLLAMA_BUILD_TESTS=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_SERVER=OFF \
    -DLLAMA_BUILD_TOOLS=OFF \
    -DLLAMA_CURL=OFF

echo "==> ビルド（llama ライブラリのみ）"
cmake --build "$BUILD_DIR" --target llama -j "$(nproc)"

echo "==> $DIST_DIR へ配置"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR/include" "$DIST_DIR/lib"
cp "$BUILD_DIR/src/libllama.a" \
   "$BUILD_DIR/ggml/src/libggml.a" \
   "$BUILD_DIR/ggml/src/libggml-base.a" \
   "$BUILD_DIR/ggml/src/libggml-cpu.a" \
   "$DIST_DIR/lib/"
cp "$LLAMA_DIR/include/llama.h" "$LLAMA_DIR"/ggml/include/*.h "$DIST_DIR/include/"

echo "==> 完了"
