# iroha-settings: 設定ウィンドウ（モデル選択・入力の設定・ユーザ辞書編集・専門辞書の追加と削除・変換ルール・
# 学習・入力履歴・変換記録）。
# macOS版の設定ウィンドウに相当。変更はconfig.json/辞書JSONへ保存し、
# 変換サーバへReloadを送って即時反映する。
cmake_minimum_required(VERSION 3.21)
project(iroha-settings LANGUAGES CXX)

add_executable(iroha-settings WIN32 main.cpp ../shared/term_dictionary_fetcher.cpp
    ../shared/typo_normalizer_fetcher.cpp ../shared/ai_client.cpp ../shared/secret_store.cpp)
target_link_libraries(iroha-settings PRIVATE
    iroha-core comctl32 uxtheme urlmon winhttp wininet bcrypt ole32 shell32 user32 gdi32 advapi32)
target_include_directories(iroha-settings PRIVATE ../shared)
target_compile_features(iroha-settings PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(iroha-settings PRIVATE /utf-8 /W4 /EHsc)
    target_compile_definitions(iroha-settings PRIVATE
        UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
endif()
