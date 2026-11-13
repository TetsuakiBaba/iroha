# iroha-cli: 変換エンジンの検証用 CLI（macOS版 iroha-cli の移植。Windows版から写したもの）
cmake_minimum_required(VERSION 3.21)
project(iroha-cli LANGUAGES CXX)

add_executable(iroha-cli main.cpp
    ../shared/term_dictionary_fetcher.cpp ../shared/typo_normalizer_fetcher.cpp)
target_link_libraries(iroha-cli PRIVATE iroha-engine)
target_include_directories(iroha-cli PRIVATE ../shared)
target_compile_options(iroha-cli PRIVATE -Wall -Wextra)
