# iroha-cli: 変換エンジンの検証用CLIハーネス（macOS版 iroha-cli のWindows移植）
cmake_minimum_required(VERSION 3.21)
project(iroha-cli LANGUAGES CXX)

add_executable(iroha-cli main.cpp)
target_link_libraries(iroha-cli PRIVATE iroha-engine)
if(MSVC)
    target_compile_options(iroha-cli PRIVATE /utf-8 /W4 /EHsc)
    target_compile_definitions(iroha-cli PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
