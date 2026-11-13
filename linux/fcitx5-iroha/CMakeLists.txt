# fcitx5-iroha: fcitx5 のエンジン（fcitx5 本体のプロセスに読み込まれるアドオン。KDE などで使う）。
# fcitx5 の開発用ファイル（libfcitx5core-dev ほか）が無ければ作らない
cmake_minimum_required(VERSION 3.21)
project(fcitx5-iroha LANGUAGES CXX)

# fcitx5 が探す場所（fcitx5 本体と同じ /usr の下）に入れる。/usr/local に入れると fcitx5 がアドオンを見つけない
set(FCITX_INSTALL_USE_FCITX_SYS_PATHS ON CACHE BOOL "fcitx5 本体と同じ場所に入れる")
find_package(Fcitx5Core QUIET)
if(NOT Fcitx5Core_FOUND)
    message(STATUS "fcitx5 の開発用ファイルが無いので、fcitx5 のエンジンは作らない（libfcitx5core-dev など）")
    return()
endif()

execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE IROHA_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT IROHA_VERSION)
    set(IROHA_VERSION "dev")
endif()
string(REGEX REPLACE "^v" "" IROHA_VERSION "${IROHA_VERSION}")

# 辞書・記号のデータは IBus のエンジンと同じインストール先から読む
set(IROHA_RESOURCE_DIR "${CMAKE_INSTALL_PREFIX}/lib/iroha")

add_library(iroha-fcitx5 MODULE engine.cpp)
set_target_properties(iroha-fcitx5 PROPERTIES PREFIX "" OUTPUT_NAME iroha)
target_link_libraries(iroha-fcitx5 PRIVATE Fcitx5::Core iroha-keys iroha-shared iroha-overlay)
target_compile_definitions(iroha-fcitx5 PRIVATE IROHA_VERSION="${IROHA_VERSION}"
                           IROHA_RESOURCE_DIR="${IROHA_RESOURCE_DIR}")
target_compile_options(iroha-fcitx5 PRIVATE -Wall -Wextra)

configure_file(iroha-addon.conf.in "${CMAKE_CURRENT_BINARY_DIR}/iroha-addon.conf" @ONLY)
configure_file(iroha-inputmethod.conf.in "${CMAKE_CURRENT_BINARY_DIR}/iroha-inputmethod.conf" @ONLY)
install(TARGETS iroha-fcitx5 DESTINATION "${FCITX_INSTALL_ADDONDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/iroha-addon.conf" DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon"
        RENAME iroha.conf)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/iroha-inputmethod.conf" DESTINATION "${FCITX_INSTALL_PKGDATADIR}/inputmethod"
        RENAME iroha.conf)
