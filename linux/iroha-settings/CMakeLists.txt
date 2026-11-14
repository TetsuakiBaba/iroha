# iroha-settings: 設定画面（GTK4）。Windows版の windows/iroha-settings を移したもの（タブ・項目・保存先を合わせる）。
# 変更は config.json・辞書の JSON などへ保存し、platform::RequestReload で IBus のエンジンに読み直してもらう。
#   settings_support  画面に依存しない部分（タブの名前・変換モデルの一覧と取得・ライセンスの一覧。単体テストする）
#   main              GTK4 の画面
#   benchmark_section 設定 > モデル > 変換のベンチマーク（開発者モード。計測は iroha-bench を起動する）
cmake_minimum_required(VERSION 3.21)
project(iroha-settings LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SETTINGS_GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(SETTINGS_CURL REQUIRED IMPORTED_TARGET libcurl)

# バージョンは git のタグから（CLAUDE.md: タグが唯一の情報源。ibus-iroha と同じやり方）
execute_process(
    COMMAND git describe --tags --match "v[0-9]*" --always --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE IROHA_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT IROHA_VERSION)
    set(IROHA_VERSION "dev")
endif()
string(REGEX REPLACE "^v" "" IROHA_VERSION "${IROHA_VERSION}")

set(IROHA_SETTINGS_WARNINGS -Wall -Wextra -Wno-missing-field-initializers)

add_library(iroha-settings-support STATIC settings_support.cpp)
target_link_libraries(iroha-settings-support PUBLIC iroha-core PRIVATE PkgConfig::SETTINGS_CURL)
target_include_directories(iroha-settings-support PUBLIC .)
target_compile_options(iroha-settings-support PRIVATE ${IROHA_SETTINGS_WARNINGS})

add_executable(iroha-settings main.cpp benchmark_section.cpp)
target_link_libraries(iroha-settings PRIVATE iroha-settings-support iroha-shared PkgConfig::SETTINGS_GTK4)
target_compile_definitions(iroha-settings PRIVATE IROHA_VERSION="${IROHA_VERSION}")
target_compile_options(iroha-settings PRIVATE ${IROHA_SETTINGS_WARNINGS})

add_executable(iroha-settings-tests tests/test_settings.cpp)
target_link_libraries(iroha-settings-tests PRIVATE iroha-settings-support)
target_compile_options(iroha-settings-tests PRIVATE ${IROHA_SETTINGS_WARNINGS})
add_test(NAME iroha-settings-tests COMMAND iroha-settings-tests)

# ---- インストール ----
# 実行ファイルは IBus・fcitx5 のエンジンと同じ場所（エンジンのプロパティから開くため）
set(IROHA_SETTINGS_DIR "${CMAKE_INSTALL_PREFIX}/lib/iroha")
set(IROHA_SETTINGS_PATH "${IROHA_SETTINGS_DIR}/iroha-settings")
configure_file(jp.iroha.Settings.desktop.in "${CMAKE_CURRENT_BINARY_DIR}/jp.iroha.Settings.desktop" @ONLY)
install(TARGETS iroha-settings RUNTIME DESTINATION "${IROHA_SETTINGS_DIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/jp.iroha.Settings.desktop"
        DESTINATION "${CMAKE_INSTALL_PREFIX}/share/applications")
