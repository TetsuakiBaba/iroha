# iroha-server: 変換サーバ常駐プロセス。
# llama.cpp + ZenzEngineをホストし、TIPからの変換要求を名前付きパイプで受ける。
# TIP DLL側にモデルを持たせないための分離（TIPは全プロセスにロードされるため）。
cmake_minimum_required(VERSION 3.21)
project(iroha-server LANGUAGES CXX)

add_executable(iroha-server main.cpp)
target_link_libraries(iroha-server PRIVATE iroha-engine)
target_include_directories(iroha-server PRIVATE ../shared)
if(MSVC)
    target_compile_options(iroha-server PRIVATE /utf-8 /W4 /EHsc)
    target_compile_definitions(iroha-server PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
