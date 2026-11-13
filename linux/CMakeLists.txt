# Linux版 iroha 全体のビルド。
#   cmake -S linux -B linux/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build linux/build
cmake_minimum_required(VERSION 3.21)
project(iroha-linux LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

enable_testing()

add_subdirectory(iroha-core)
add_subdirectory(iroha-cli)
