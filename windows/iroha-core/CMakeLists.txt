# iroha-core: 変換エンジンのC++移植。正は macos/Sources/IrohaCore/。
#   iroha-core   — 純粋ロジック（llama.cpp非依存。TIPにもサーバにもリンク可）
#   iroha-engine — ZenzEngine（llama.cpp依存。サーバ/CLI専用。TIPにはリンクしない）
cmake_minimum_required(VERSION 3.21)
project(iroha-core LANGUAGES CXX)

add_library(iroha-core STATIC
    src/json.cpp
    src/kana.cpp
    src/learning_dictionary.cpp
    src/learning_engine.cpp
    src/learning_store.cpp
    src/reading_aligner.cpp
    src/reading_constraint.cpp
    src/romaji_composer.cpp
    src/unicode.cpp
    src/user_dictionary.cpp
    src/user_dictionary_engine.cpp
    src/user_dictionary_store.cpp
)
target_compile_features(iroha-core PUBLIC cxx_std_17)
target_include_directories(iroha-core PUBLIC include)
if(MSVC)
    target_compile_options(iroha-core PRIVATE /utf-8 /W4 /EHsc)
endif()

# llama.cpp静的ライブラリ（windows/scripts/build-llama.ps1 が生成。
# IROHA_VULKAN=ON なら -Backend vulkan で生成した dist-windows-vulkan を使う）
option(IROHA_VULKAN "Vulkan対応のllama.cppとリンクする（GPUが無ければ実行時にCPUへフォールバック）" OFF)
if(IROHA_VULKAN)
    set(IROHA_DIST "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/dist-windows-vulkan")
else()
    set(IROHA_DIST "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/dist-windows")
endif()
if(NOT EXISTS "${IROHA_DIST}/lib/llama.lib")
    message(FATAL_ERROR
        "${IROHA_DIST} にllama.cppがありません。"
        "先に windows/scripts/build-llama.ps1 を実行してください"
        "（IROHA_VULKAN=ONの場合は -Backend vulkan 付きで）")
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
if(IROHA_VULKAN)
    if(NOT DEFINED ENV{VULKAN_SDK})
        message(FATAL_ERROR "IROHA_VULKAN=ONにはVULKAN_SDK環境変数が必要です")
    endif()
    target_link_libraries(iroha-engine PUBLIC
        "${IROHA_DIST}/lib/ggml-vulkan.lib"
        "$ENV{VULKAN_SDK}/Lib/vulkan-1.lib"
    )
endif()
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
