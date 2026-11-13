# iroha-server: 変換サーバ常駐プロセス。
# llama.cpp + ZenzEngineをホストし、TIPからの変換要求を名前付きパイプで受ける。
# TIP DLL側にモデルを持たせないための分離（TIPは全プロセスにロードされるため）。
cmake_minimum_required(VERSION 3.21)
project(iroha-server LANGUAGES CXX)

add_executable(iroha-server main.cpp ../shared/emoji_renderability.cpp ../shared/ai_client.cpp
    ../shared/secret_store.cpp)
target_link_libraries(iroha-server PRIVATE iroha-engine dwrite winhttp advapi32)
target_include_directories(iroha-server PRIVATE ../shared)
if(MSVC)
    target_compile_options(iroha-server PRIVATE /utf-8 /W4 /EHsc)
    target_compile_definitions(iroha-server PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
