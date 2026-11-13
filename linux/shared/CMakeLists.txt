# iroha-shared: OS の部品に頼る共有の部品（IBus のエンジン・fcitx5 のエンジン・設定画面・iroha-cli が使う）
#   engine_host            設定からエンジン一式を組み立てる（SessionManager の Backend）
#   ai_client              AI変換・AIで直す・選択テキストのAI編集の通信（libcurl）
#   secret_store           API キーの保存（libsecret）
#   term_dictionary_fetcher / typo_normalizer_fetcher  専門辞書・打ち間違い訂正モデルの取得（libcurl）
#   emoji_renderability    描けない絵文字の判定（fontconfig・FreeType・HarfBuzz）
#   launch_log             起動・終了・異常終了の記録
#   key_shortcut.h         ショートカットの文字列
cmake_minimum_required(VERSION 3.21)
project(iroha-shared LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(SECRET REQUIRED IMPORTED_TARGET libsecret-1)
pkg_check_modules(FONTS REQUIRED IMPORTED_TARGET fontconfig freetype2 harfbuzz)

add_library(iroha-shared STATIC
    ai_client.cpp
    emoji_renderability.cpp
    engine_host.cpp
    launch_log.cpp
    secret_store.cpp
    term_dictionary_fetcher.cpp
    typo_normalizer_fetcher.cpp
)
target_include_directories(iroha-shared PUBLIC .)
target_link_libraries(iroha-shared PUBLIC iroha-engine PkgConfig::CURL PkgConfig::SECRET PRIVATE PkgConfig::FONTS)
target_compile_options(iroha-shared PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
set_target_properties(iroha-shared PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(iroha-shared-tests tests/test_shared.cpp)
target_link_libraries(iroha-shared-tests PRIVATE iroha-shared)
target_compile_options(iroha-shared-tests PRIVATE -Wall -Wextra)
add_test(NAME iroha-shared-tests COMMAND iroha-shared-tests)
