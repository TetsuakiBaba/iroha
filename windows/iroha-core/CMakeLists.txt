# iroha-core: 変換エンジンのC++移植。正は macos/Sources/IrohaCore/。
# Windows APIに依存しない（TIPにもサーバにもリンクされる）。
cmake_minimum_required(VERSION 3.21)
project(iroha-core LANGUAGES CXX)

add_library(iroha-core STATIC
    src/kana.cpp
    src/romaji_composer.cpp
    src/unicode.cpp
)
target_compile_features(iroha-core PUBLIC cxx_std_17)
target_include_directories(iroha-core PUBLIC include)
if(MSVC)
    target_compile_options(iroha-core PRIVATE /utf-8 /W4 /EHsc)
endif()

add_executable(iroha-core-tests tests/test_main.cpp)
target_link_libraries(iroha-core-tests PRIVATE iroha-core)
if(MSVC)
    target_compile_options(iroha-core-tests PRIVATE /utf-8 /W4 /EHsc)
endif()

add_test(NAME iroha-core-tests COMMAND iroha-core-tests)
