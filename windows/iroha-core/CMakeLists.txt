# iroha-core: 変換エンジンのC++移植。正は macos/Sources/IrohaCore/。
#   iroha-core   — 純粋ロジック（llama.cpp非依存。TIPにもサーバにもリンク可）
#   iroha-engine — ZenzEngine（llama.cpp依存。サーバ/CLI専用。TIPにはリンクしない）
cmake_minimum_required(VERSION 3.21)
project(iroha-core LANGUAGES CXX)

add_library(iroha-core STATIC
    src/kana.cpp
    src/reading_aligner.cpp
    src/reading_constraint.cpp
    src/romaji_composer.cpp
    src/unicode.cpp
)
target_compile_features(iroha-core PUBLIC cxx_std_17)
target_include_directories(iroha-core PUBLIC include)
if(MSVC)
    target_compile_options(iroha-core PRIVATE /utf-8 /W4 /EHsc)
endif()

# llama.cpp静的ライブラリ（windows/scripts/build-llama.ps1 が vendor/dist-windows に生成）
set(IROHA_DIST "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/dist-windows")
if(NOT EXISTS "${IROHA_DIST}/lib/llama.lib")
    message(FATAL_ERROR
        "vendor/dist-windows にllama.cppがありません。"
        "先に windows/scripts/build-llama.ps1 を実行してください")
endif()

add_library(iroha-engine STATIC
    src/zenz_engine.cpp
)
target_link_libraries(iroha-engine PUBLIC iroha-core)
target_include_directories(iroha-engine PRIVATE "${IROHA_DIST}/include")
target_link_libraries(iroha-engine PUBLIC
    "${IROHA_DIST}/lib/llama.lib"
    "${IROHA_DIST}/lib/ggml.lib"
    "${IROHA_DIST}/lib/ggml-cpu.lib"
    "${IROHA_DIST}/lib/ggml-base.lib"
)
if(MSVC)
    target_compile_options(iroha-engine PRIVATE /utf-8 /W4 /EHsc)
    target_compile_definitions(iroha-engine PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(iroha-core-tests tests/test_main.cpp)
target_link_libraries(iroha-core-tests PRIVATE iroha-core)
if(MSVC)
    target_compile_options(iroha-core-tests PRIVATE /utf-8 /W4 /EHsc)
endif()

add_test(NAME iroha-core-tests COMMAND iroha-core-tests)
