# iroha-settings: 設定ウィンドウ（モデル選択・ユーザ辞書編集・学習リセット）。
# macOS版の設定ウィンドウに相当。変更はconfig.json/辞書JSONへ保存し、
# 変換サーバへReloadを送って即時反映する。
cmake_minimum_required(VERSION 3.21)
project(iroha-settings LANGUAGES CXX)

add_executable(iroha-settings WIN32 main.cpp)
target_link_libraries(iroha-settings PRIVATE
    iroha-core comctl32 urlmon ole32 shell32 user32 gdi32 advapi32)
target_include_directories(iroha-settings PRIVATE ../shared)
target_compile_features(iroha-settings PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(iroha-settings PRIVATE /utf-8 /W4 /EHsc)
    target_compile_definitions(iroha-settings PRIVATE
        UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
endif()
