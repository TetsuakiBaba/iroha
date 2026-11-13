# iroha-core: 変換エンジンの C++ 実装（Linux版）。
#   Windows版の windows/iroha-core をコミット 2de5ee1 で写したもので、以後は Linux版として別に保つ。
#   仕様の正は macos/Sources/IrohaCore/。
#   iroha-core   — 純粋ロジック（llama.cpp 非依存）
#   iroha-engine — ZenzEngine（llama.cpp 依存）
cmake_minimum_required(VERSION 3.21)
project(iroha-core LANGUAGES CXX)

set(IROHA_WARNINGS -Wall -Wextra)

add_library(iroha-core STATIC
    src/chunked_conversion_engine.cpp
    src/config.cpp
    src/input_history.cpp
    src/conversion_log.cpp
    src/input_session.cpp
    src/json.cpp
    src/kana.cpp
    src/lattice_converter.cpp
    src/lattice_rescoring_engine.cpp
    src/lattice/dicdata.cpp
    src/lattice/dicdata_store.cpp
    src/lattice/kana_kanji_converter.cpp
    src/lattice/louds.cpp
    src/lattice/number.cpp
    src/lattice/special_candidates.cpp
    src/lattice/term_index.cpp
    src/learning_dictionary.cpp
    src/learning_engine.cpp
    src/learning_store.cpp
    src/platform.cpp
    src/reading_aligner.cpp
    src/reading_constraint.cpp
    src/romaji_composer.cpp
    src/session_manager.cpp
    src/sha256.cpp
    src/symbol_candidates.cpp
    src/term_dictionary.cpp
    src/typo_normalizer.cpp
    src/typo_normalizer_catalog.cpp
    src/unicode.cpp
    src/user_dictionary.cpp
    src/user_dictionary_engine.cpp
    src/user_dictionary_store.cpp
    src/user_rewrite_rule.cpp
    src/variant_kanji_engine.cpp
)
target_compile_features(iroha-core PUBLIC cxx_std_17)
target_include_directories(iroha-core PUBLIC include)
# src/lattice/ の内部ヘッダ（"lattice/…" で参照する。外には公開しない）
target_include_directories(iroha-core PRIVATE src)
target_compile_options(iroha-core PRIVATE ${IROHA_WARNINGS})
set_target_properties(iroha-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_package(Threads REQUIRED)
target_link_libraries(iroha-core PUBLIC Threads::Threads)

# llama.cpp 静的ライブラリ（linux/scripts/build-llama.sh が生成）
set(IROHA_DIST "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/dist-linux")
if(NOT EXISTS "${IROHA_DIST}/lib/libllama.a")
    message(FATAL_ERROR
        "${IROHA_DIST} に llama.cpp がありません。先に linux/scripts/build-llama.sh を実行してください")
endif()

add_library(iroha-engine STATIC
    src/engine_host.cpp
    src/zenz_engine.cpp
)
target_link_libraries(iroha-engine PUBLIC iroha-core)
target_include_directories(iroha-engine PRIVATE "${IROHA_DIST}/include")
target_compile_options(iroha-engine PRIVATE ${IROHA_WARNINGS})
set_target_properties(iroha-engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
# 静的ライブラリどうしの参照の順（llama → ggml → ggml-cpu → ggml-base）
target_link_libraries(iroha-engine PUBLIC
    "${IROHA_DIST}/lib/libllama.a"
    "${IROHA_DIST}/lib/libggml.a"
    "${IROHA_DIST}/lib/libggml-cpu.a"
    "${IROHA_DIST}/lib/libggml-base.a"
    ${CMAKE_DL_LIBS}
    m
)

add_executable(iroha-core-tests tests/test_main.cpp)
target_link_libraries(iroha-core-tests PRIVATE iroha-core)
# 辞書ラティスの部品（src/lattice/ の内部ヘッダ）を直接テストする
target_include_directories(iroha-core-tests PRIVATE src)
target_compile_options(iroha-core-tests PRIVATE ${IROHA_WARNINGS})

add_test(NAME iroha-core-tests COMMAND iroha-core-tests)
