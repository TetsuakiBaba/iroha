# iroha-tip: TSFテキストサービスDLL。
# 入力先の全プロセスにロードされるため、依存は最小に保つ
# （変換エンジン・llama.cppは絶対にここへリンクしない。iroha-server側に置く）。
cmake_minimum_required(VERSION 3.21)
project(iroha-tip LANGUAGES CXX RC)

add_library(iroha-tip SHARED
    src/CandidateWindow.cpp
    src/Composition.cpp
    src/ConvertClient.cpp
    src/DisplayAttribute.cpp
    src/DllMain.cpp
    src/Globals.cpp
    src/LangBarButton.cpp
    src/Registration.cpp
    src/TextService.cpp
    src/iroha-tip.def
    src/iroha-tip.rc
)
target_include_directories(iroha-tip PRIVATE ../shared)

target_compile_features(iroha-tip PRIVATE cxx_std_17)
target_compile_definitions(iroha-tip PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_compile_options(iroha-tip PRIVATE /utf-8 /W4 /EHsc)
# iroha-coreからは入力合成（RomajiComposer）と文字コード変換のみ使う。
# 変換エンジン・llama.cpp関連は将来もリンクしないこと。
target_link_libraries(iroha-tip PRIVATE iroha-core ole32 oleaut32 uuid advapi32 user32)
