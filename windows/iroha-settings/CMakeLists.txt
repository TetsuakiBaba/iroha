# iroha-settings: 設定ウィンドウ（Windows 11 の設定アプリの見た目。左に項目の一覧、右に項目のカード）。
# macOS版の設定ウィンドウに相当。変更は config.json・辞書の JSON へすぐ保存し、
# 変換サーバへ Reload を送って反映する。
cmake_minimum_required(VERSION 3.21)
project(iroha-settings LANGUAGES CXX)

add_executable(iroha-settings WIN32 main.cpp ui.cpp ../shared/term_dictionary_fetcher.cpp
    ../shared/typo_normalizer_fetcher.cpp ../shared/ai_client.cpp ../shared/secret_store.cpp
    ../shared/voice_recognizer.cpp)
target_link_libraries(iroha-settings PRIVATE
    iroha-core comctl32 uxtheme urlmon winhttp wininet bcrypt ole32 shell32 user32 gdi32 gdiplus comdlg32
    advapi32 sapi winmm)
target_include_directories(iroha-settings PRIVATE ../shared)
target_compile_features(iroha-settings PRIVATE cxx_std_17)
# 情報のページに出す版（構成したときの git describe。リリースのビルドではタグの名前になる）
execute_process(COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE IROHA_SETTINGS_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(IROHA_SETTINGS_VERSION)
    target_compile_definitions(iroha-settings PRIVATE IROHA_VERSION="${IROHA_SETTINGS_VERSION}")
endif()
if(MSVC)
    target_compile_options(iroha-settings PRIVATE /utf-8 /W4 /EHsc)
    target_compile_definitions(iroha-settings PRIVATE
        UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
endif()
