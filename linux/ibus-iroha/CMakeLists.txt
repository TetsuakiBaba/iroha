# ibus-iroha: IBus のエンジン（ibus-daemon が起動するプロセス。変換エンジン一式を同じプロセスに持つ）
cmake_minimum_required(VERSION 3.21)
project(ibus-iroha LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(IBUS REQUIRED IMPORTED_TARGET ibus-1.0)

# バージョンは git のタグから（CLAUDE.md: タグが唯一の情報源）
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE IROHA_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT IROHA_VERSION)
    set(IROHA_VERSION "dev")
endif()
string(REGEX REPLACE "^v" "" IROHA_VERSION "${IROHA_VERSION}")

# キーの読み替え（IBus に依存しない。単体テストする）
add_library(ibus-iroha-keys STATIC key_decision.cpp)
target_link_libraries(ibus-iroha-keys PUBLIC iroha-core)
target_include_directories(ibus-iroha-keys PUBLIC .)
target_compile_options(ibus-iroha-keys PRIVATE -Wall -Wextra)

add_executable(ibus-engine-iroha main.cpp engine.cpp)
target_link_libraries(ibus-engine-iroha PRIVATE ibus-iroha-keys iroha-engine PkgConfig::IBUS)
target_compile_definitions(ibus-engine-iroha PRIVATE IROHA_VERSION="${IROHA_VERSION}")
target_compile_options(ibus-engine-iroha PRIVATE -Wall -Wextra)

add_executable(ibus-iroha-tests tests/test_key_decision.cpp)
target_link_libraries(ibus-iroha-tests PRIVATE ibus-iroha-keys)
target_compile_options(ibus-iroha-tests PRIVATE -Wall -Wextra)
add_test(NAME ibus-iroha-tests COMMAND ibus-iroha-tests)

# ---- インストール ----
# 実行ファイルの隣に辞書ラティスの辞書（Dictionary）と記号のデータ（Symbols）を置く（ExecutableDirectory から探す）
set(IROHA_ENGINE_DIR "${CMAKE_INSTALL_PREFIX}/lib/ibus-iroha")
set(IROHA_ENGINE_PATH "${IROHA_ENGINE_DIR}/ibus-engine-iroha")
# コンポーネントの定義は ibus-daemon が読む場所に置く（既定は IBus 自身のデータの場所。/usr/share/ibus/component）
pkg_get_variable(IBUS_PKGDATADIR ibus-1.0 pkgdatadir)
set(IROHA_IBUS_COMPONENT_DIR "${IBUS_PKGDATADIR}/component" CACHE PATH "IBus のコンポーネントの定義を置く場所")
configure_file(iroha.xml.in "${CMAKE_CURRENT_BINARY_DIR}/iroha.xml" @ONLY)

set(IROHA_VENDOR "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor")
install(TARGETS ibus-engine-iroha RUNTIME DESTINATION "${IROHA_ENGINE_DIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/iroha.xml" DESTINATION "${IROHA_IBUS_COMPONENT_DIR}")
install(DIRECTORY "${IROHA_VENDOR}/azooKey_dictionary_storage/Dictionary/" DESTINATION "${IROHA_ENGINE_DIR}/Dictionary")
install(FILES "${IROHA_VENDOR}/azooKey_dictionary_storage/LICENSE"
        DESTINATION "${IROHA_ENGINE_DIR}/Dictionary" RENAME LICENSE-azooKey_dictionary_storage)
install(DIRECTORY "${IROHA_VENDOR}/mozc-data/" DESTINATION "${IROHA_ENGINE_DIR}/Symbols")
