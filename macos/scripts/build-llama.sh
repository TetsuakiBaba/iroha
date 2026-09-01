#!/bin/bash
# llama.cppをスタティックビルドして vendor/dist に配置する。
# 前提: brew install cmake
set -euo pipefail
# vendor/ と patches/ はリポジトリルートにあるため、そこへ移動する
cd "$(dirname "$0")/../.."

LLAMA_TAG="b10689"

if [ ! -d vendor/llama.cpp ]; then
    echo "==> llama.cpp ($LLAMA_TAG) を取得"
    git clone --depth 1 --branch "$LLAMA_TAG" https://github.com/ggml-org/llama.cpp vendor/llama.cpp
fi

# zenzのpre-tokenizer名を認識させるパッチ（適用済みならスキップ）
if ! grep -q "gpt2-small-japanese-char" vendor/llama.cpp/src/llama-vocab.cpp; then
    echo "==> zenz対応パッチを適用"
    git -C vendor/llama.cpp apply ../../patches/llama-cpp-zenz-pretokenizer.patch
fi

echo "==> CMake configure"
cmake -S vendor/llama.cpp -B vendor/llama.cpp/build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_METAL=ON \
    -DGGML_METAL_EMBED_LIBRARY=ON \
    -DLLAMA_BUILD_TESTS=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_SERVER=OFF \
    -DLLAMA_BUILD_TOOLS=OFF \
    -DLLAMA_CURL=OFF

echo "==> ビルド（llamaライブラリのみ）"
cmake --build vendor/llama.cpp/build --target llama -j "$(sysctl -n hw.ncpu)"

echo "==> vendor/dist へ配置"
mkdir -p vendor/dist/include vendor/dist/lib
cp vendor/llama.cpp/build/src/libllama.a \
   vendor/llama.cpp/build/ggml/src/libggml.a \
   vendor/llama.cpp/build/ggml/src/libggml-base.a \
   vendor/llama.cpp/build/ggml/src/libggml-cpu.a \
   vendor/llama.cpp/build/ggml/src/ggml-metal/libggml-metal.a \
   vendor/llama.cpp/build/ggml/src/ggml-blas/libggml-blas.a \
   vendor/dist/lib/
cp vendor/llama.cpp/include/llama.h vendor/llama.cpp/ggml/include/*.h vendor/dist/include/

echo "==> 完了"
