# iroha-cli: 変換エンジンの検証用CLIハーネス（macOS版 iroha-cli のWindows移植）
cmake_minimum_required(VERSION 3.21)
project(iroha-cli LANGUAGES CXX)

add_executable(iroha-cli main.cpp ../shared/emoji_renderability.cpp
    ../shared/term_dictionary_fetcher.cpp ../shared/typo_normalizer_fetcher.cpp
    ../shared/ai_client.cpp ../shared/secret_store.cpp ../shared/voice_recognizer.cpp)
target_link_libraries(iroha-cli PRIVATE iroha-engine dwrite urlmon wininet bcrypt winhttp advapi32 ole32 sapi winmm)
target_include_directories(iroha-cli PRIVATE ../shared)
if(MSVC)
    target_compile_options(iroha-cli PRIVATE /utf-8 /W4 /EHsc)
    target_compile_definitions(iroha-cli PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
