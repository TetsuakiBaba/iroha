# iroha-bench: 変換のベンチマークを測るヘルパー（設定画面が起動する。macOS版・Windows版の iroha-bench と同じ引数・出力）
cmake_minimum_required(VERSION 3.21)
project(iroha-bench LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
# 機械の状態: 省電力（power-profiles-daemon の D-Bus）と、計測中の入力（X11。libXss は実行時に dlopen で読む）
pkg_check_modules(BENCH REQUIRED IMPORTED_TARGET gio-2.0 x11)

# バージョンは git のタグから（CLAUDE.md: タグが唯一の情報源。データの Release のタグ benchmarks-v1 などを拾わないよう v で始まるものだけ）
execute_process(
    COMMAND git describe --tags --match "v[0-9]*" --always --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE IROHA_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT IROHA_VERSION)
    set(IROHA_VERSION "dev")
endif()
string(REGEX REPLACE "^v" "" IROHA_VERSION "${IROHA_VERSION}")

add_executable(iroha-bench main.cpp)
target_link_libraries(iroha-bench PRIVATE iroha-engine PkgConfig::BENCH ${CMAKE_DL_LIBS})
target_compile_definitions(iroha-bench PRIVATE IROHA_VERSION="${IROHA_VERSION}")
target_compile_options(iroha-bench PRIVATE -Wall -Wextra)

# 実行ファイルの隣に辞書（Dictionary）を置く場所と同じ所へ入れる（ibus-iroha の CMakeLists.txt のインストール先と同じ）
install(TARGETS iroha-bench RUNTIME DESTINATION lib/iroha)
