# iroha-settings: 設定ウィンドウ（Windows 11 の設定アプリの見た目。左に項目の一覧、右に項目のカード）。
# macOS版の設定ウィンドウに相当。変更は config.json・辞書の JSON へすぐ保存し、
# 変換サーバへ Reload を送って反映する。
cmake_minimum_required(VERSION 3.21)
project(iroha-settings LANGUAGES CXX)

add_executable(iroha-settings WIN32 main.cpp ui.cpp updater.cpp key_check.cpp ../shared/term_dictionary_fetcher.cpp
    ../shared/typo_normalizer_fetcher.cpp ../shared/ai_client.cpp ../shared/secret_store.cpp
    ../shared/voice_recognizer.cpp)
target_link_libraries(iroha-settings PRIVATE
    iroha-core comctl32 uxtheme urlmon winhttp wininet bcrypt ole32 shell32 user32 gdi32 gdiplus comdlg32
    advapi32 sapi winmm)
target_include_directories(iroha-settings PRIVATE ../shared)
target_compile_features(iroha-settings PRIVATE cxx_std_17)
# 情報のページに出す版・アップデートの確認で比べる版（IROHA_VERSION）とアイコン
iroha_version_resource(iroha-settings "iroha 設定" "iroha-settings.exe" "VFT_APP" TRUE)
if(MSVC)
    target_compile_options(iroha-settings PRIVATE /utf-8 /W4 /EHsc)
    target_compile_definitions(iroha-settings PRIVATE
        UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
endif()
