# Windows版iroha全体のビルド。
#   cmake -S windows -B windows/build -G Ninja && cmake --build windows/build
cmake_minimum_required(VERSION 3.21)
project(iroha-windows LANGUAGES CXX)

enable_testing()

add_subdirectory(iroha-core)
add_subdirectory(iroha-tip)
