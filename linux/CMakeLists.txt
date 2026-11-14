# Linux版 iroha 全体のビルド。
#   cmake -S linux -B linux/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build linux/build
cmake_minimum_required(VERSION 3.21)
project(iroha-linux LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

enable_testing()

add_subdirectory(iroha-core)
add_subdirectory(shared)
add_subdirectory(iroha-cli)
add_subdirectory(iroha-bench)
add_subdirectory(ibus-iroha)
add_subdirectory(fcitx5-iroha)
# 設定画面と選択テキストのAI編集（GTK4）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/iroha-settings/CMakeLists.txt")
    add_subdirectory(iroha-settings)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/iroha-selection/CMakeLists.txt")
    add_subdirectory(iroha-selection)
endif()
