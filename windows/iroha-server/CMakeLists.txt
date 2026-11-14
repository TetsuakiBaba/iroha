# iroha-server: 変換サーバ常駐プロセス。
# llama.cpp + ZenzEngineをホストし、TIPからの変換要求を名前付きパイプで受ける。
# TIP DLL側にモデルを持たせないための分離（TIPは全プロセスにロードされるため）。
cmake_minimum_required(VERSION 3.21)
project(iroha-server LANGUAGES CXX)

add_executable(iroha-server main.cpp selection_assistant.cpp ../shared/emoji_renderability.cpp
    ../shared/ai_client.cpp ../shared/secret_store.cpp ../shared/voice_recognizer.cpp)
# 窓のないプログラムにする（コンソールにすると、ログオン時の自動起動で黒い窓が出る）。入口は wmain のまま
set_target_properties(iroha-server PROPERTIES WIN32_EXECUTABLE TRUE)
iroha_version_resource(iroha-server "iroha 変換サーバ" "iroha-server.exe" "VFT_APP" TRUE)
target_link_libraries(iroha-server PRIVATE iroha-engine dwrite winhttp advapi32 user32 ole32 sapi winmm)
target_include_directories(iroha-server PRIVATE ../shared)
if(MSVC)
    target_compile_options(iroha-server PRIVATE /utf-8 /W4 /EHsc)
    target_link_options(iroha-server PRIVATE /ENTRY:wmainCRTStartup)
    target_compile_definitions(iroha-server PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
