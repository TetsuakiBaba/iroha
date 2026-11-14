# Windows版iroha全体のビルド。
#   cmake -S windows -B windows/build -G Ninja && cmake --build windows/build
cmake_minimum_required(VERSION 3.21)
project(iroha-windows LANGUAGES CXX RC)

enable_testing()
set(IROHA_WINDOWS_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# 版。リリースは CI がタグから渡す（-DIROHA_VERSION=0.18.0）。空なら git describe（v で始まるタグだけ）から取る。
# 開発版は「0.17.1-29-gcf75edd-dirty」の形になる（アップデートの確認は 0.17.1 として比べる）
set(IROHA_VERSION "" CACHE STRING "版（空なら git describe から）")
set(IROHA_VERSION_TEXT "${IROHA_VERSION}")
if(IROHA_VERSION_TEXT STREQUAL "")
    execute_process(COMMAND git describe --tags --match "v[0-9]*" --always --dirty
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE IROHA_VERSION_TEXT
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
string(REGEX REPLACE "^v" "" IROHA_VERSION_TEXT "${IROHA_VERSION_TEXT}")
if(NOT IROHA_VERSION_TEXT MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
    set(IROHA_VERSION_TEXT "0.0.0-dev")
endif()
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" IROHA_VERSION_CORE "${IROHA_VERSION_TEXT}")
set(IROHA_VERSION_MAJOR ${CMAKE_MATCH_1})
set(IROHA_VERSION_MINOR ${CMAKE_MATCH_2})
set(IROHA_VERSION_PATCH ${CMAKE_MATCH_3})
message(STATUS "iroha version: ${IROHA_VERSION_TEXT}")

# 実行ファイル・DLL に版の情報（とアイコン）を入れ、IROHA_VERSION を定義する
function(iroha_version_resource target description filename filetype with_icon)
    set(IROHA_RC_DESCRIPTION "${description}")
    set(IROHA_RC_FILENAME "${filename}")
    set(IROHA_RC_FILETYPE "${filetype}")
    if(with_icon)
        set(IROHA_RC_ICON "1 ICON \"${IROHA_WINDOWS_DIR}/resources/iroha.ico\"")
    else()
        set(IROHA_RC_ICON "")
    endif()
    configure_file(${IROHA_WINDOWS_DIR}/shared/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/${target}-version.rc @ONLY)
    target_sources(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${target}-version.rc)
    target_compile_definitions(${target} PRIVATE IROHA_VERSION="${IROHA_VERSION_TEXT}")
endfunction()

add_subdirectory(iroha-core)
add_subdirectory(iroha-bench)
add_subdirectory(iroha-cli)
add_subdirectory(iroha-server)
add_subdirectory(iroha-settings)
add_subdirectory(iroha-tip)
