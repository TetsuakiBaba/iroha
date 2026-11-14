# iroha-bench: 変換のベンチマークを測るヘルパー（設定画面が起動する。macOS版 iroha-bench と同じ引数・出力）
cmake_minimum_required(VERSION 3.21)
project(iroha-bench LANGUAGES CXX)

add_executable(iroha-bench main.cpp)
iroha_version_resource(iroha-bench "iroha 変換のベンチマーク" "iroha-bench.exe" "VFT_APP" TRUE)
target_link_libraries(iroha-bench PRIVATE iroha-engine advapi32 user32)
if(MSVC)
    target_compile_options(iroha-bench PRIVATE /utf-8 /W4 /EHsc)
    target_compile_definitions(iroha-bench PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
