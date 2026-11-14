# iroha-selection: 選択テキストのAI編集の常駐プログラム（GTK4、X11）
#   selection_rules  設定の読み方・使わないアプリの照合・ショートカットから X のキーへの変換・依頼の組み立て
#                    （画面と X に頼らない。単体テストする）
#   x11_desktop      ショートカットをつかむ・前面の窓・Ctrl+V を送る（XTest）など X に頼む部分
#   selection_app    GtkApplication（常駐・設定の見張り・PRIMARY 選択・結果の窓・AI への依頼・貼り付け）
cmake_minimum_required(VERSION 3.21)
project(iroha-selection LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11 xtst xi xfixes)

# バージョンは git のタグから（CLAUDE.md: タグが唯一の情報源）
execute_process(
    COMMAND git describe --tags --match "v[0-9]*" --always --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE IROHA_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT IROHA_VERSION)
    set(IROHA_VERSION "dev")
endif()
string(REGEX REPLACE "^v" "" IROHA_VERSION "${IROHA_VERSION}")

set(IROHA_SELECTION_WARNINGS -Wall -Wextra -Wno-missing-field-initializers)

add_library(iroha-selection-rules STATIC selection_rules.cpp)
target_link_libraries(iroha-selection-rules PUBLIC iroha-core)
target_include_directories(iroha-selection-rules PUBLIC . ../shared)
target_compile_options(iroha-selection-rules PRIVATE ${IROHA_SELECTION_WARNINGS})

add_executable(iroha-selection main.cpp selection_app.cpp x11_desktop.cpp)
target_link_libraries(iroha-selection PRIVATE iroha-selection-rules iroha-shared PkgConfig::GTK4 PkgConfig::X11)
target_compile_definitions(iroha-selection PRIVATE IROHA_VERSION="${IROHA_VERSION}")
target_compile_options(iroha-selection PRIVATE ${IROHA_SELECTION_WARNINGS})

add_executable(iroha-selection-tests tests/test_selection_rules.cpp)
target_link_libraries(iroha-selection-tests PRIVATE iroha-selection-rules)
target_compile_options(iroha-selection-tests PRIVATE ${IROHA_SELECTION_WARNINGS})
add_test(NAME iroha-selection-tests COMMAND iroha-selection-tests)

# ---- インストール ----
# 実行ファイルは IBus・fcitx5 のエンジンと同じ場所。ログインのときに自動で起動する（selectionEnabled が偽なら
# 起動しても何もしないので、常に入れる）
set(IROHA_SELECTION_DIR "${CMAKE_INSTALL_PREFIX}/lib/iroha")
set(IROHA_SELECTION_PATH "${IROHA_SELECTION_DIR}/iroha-selection")
set(IROHA_AUTOSTART_DIR "/etc/xdg/autostart" CACHE PATH "ログインのときに起動するものの定義を置く場所")
configure_file(jp.iroha.Selection.desktop.in "${CMAKE_CURRENT_BINARY_DIR}/jp.iroha.Selection.desktop" @ONLY)

install(TARGETS iroha-selection RUNTIME DESTINATION "${IROHA_SELECTION_DIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/jp.iroha.Selection.desktop" DESTINATION "${IROHA_AUTOSTART_DIR}")
