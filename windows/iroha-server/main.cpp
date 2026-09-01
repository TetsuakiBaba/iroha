// iroha変換サーバ。
// 名前付きパイプでTIP（各アプリのプロセス内）からの変換要求を受け、
// ZenzEngine（llama.cpp + GGUFモデル）で変換して返す。
//
// - 単一インスタンス（セッション別の名前付きミューテックスでガード）
// - 単一スレッドで1接続ずつ処理する（エンジンの直列化を兼ねる）
// - モデルは起動時にプリロードする（初回変換のもたつき防止）

#include "ipc_protocol.h"

#include <cstdio>
#include <string>
#include <vector>

#include "iroha/unicode.h"
#include "iroha/zenz_engine.h"

namespace {

void Log(const char* message) {
    std::printf("%s\n", message);
    OutputDebugStringA((std::string("[iroha-server] ") + message + "\n").c_str());
}

std::string DefaultModelPath() {
    if (const wchar_t* env = _wgetenv(L"IROHA_MODEL")) {
        return iroha::Utf32ToUtf8(iroha::Utf16ToUtf32(env));
    }
    const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
    const std::wstring base = localAppData ? localAppData : L".";
    return iroha::Utf32ToUtf8(
        iroha::Utf16ToUtf32(base + L"\\iroha\\models\\zenz-v3.1-small-Q5_K_M.gguf"));
}

// 1リクエストを処理してレスポンスのバイト列を返す。shutdownならfalseを返す
bool HandleRequest(iroha::ZenzEngine& engine, const std::vector<char>& request,
                   std::vector<char>* response) {
    iroha::ipc::Reader reader{request.data(), request.size()};
    uint32_t version = 0;
    uint32_t type = 0;
    if (!reader.U32(&version) || version != iroha::ipc::kProtocolVersion ||
        !reader.U32(&type)) {
        *response = iroha::ipc::BuildErrorResponse("プロトコルが不正です");
        return true;
    }
    switch (static_cast<iroha::ipc::MessageType>(type)) {
        case iroha::ipc::MessageType::Ping:
            *response = iroha::ipc::BuildOkResponse({});
            return true;
        case iroha::ipc::MessageType::Shutdown:
            *response = iroha::ipc::BuildOkResponse({});
            return false;
        case iroha::ipc::MessageType::Convert: {
            uint32_t candidateCount = 0;
            std::string readingUtf8;
            std::string contextUtf8;
            if (!reader.U32(&candidateCount) || !reader.String(&readingUtf8) ||
                !reader.String(&contextUtf8)) {
                *response = iroha::ipc::BuildErrorResponse("プロトコルが不正です");
                return true;
            }
            candidateCount = candidateCount == 0 ? 1 : candidateCount;
            candidateCount = candidateCount > 32 ? 32 : candidateCount;
            std::vector<std::u32string> candidates;
            std::string error;
            if (!engine.Convert(iroha::Utf8ToUtf32(readingUtf8),
                                iroha::Utf8ToUtf32(contextUtf8),
                                static_cast<int>(candidateCount), &candidates,
                                &error)) {
                Log(("convert failed: " + error).c_str());
                *response = iroha::ipc::BuildErrorResponse(error);
                return true;
            }
            std::vector<std::string> utf8Candidates;
            utf8Candidates.reserve(candidates.size());
            for (const auto& candidate : candidates) {
                utf8Candidates.push_back(iroha::Utf32ToUtf8(candidate));
            }
            *response = iroha::ipc::BuildOkResponse(utf8Candidates);
            return true;
        }
        default:
            *response = iroha::ipc::BuildErrorResponse("未知のメッセージ種別です");
            return true;
    }
}

} // namespace

int wmain() {
    SetConsoleOutputCP(CP_UTF8);

    // 単一インスタンスガード（セッション別）
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    const std::wstring mutexName = L"Local\\iroha-server-" + std::to_wstring(sessionId);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        Log("already running");
        return 0;
    }

    iroha::ZenzEngine engine(DefaultModelPath());
    {
        std::string error;
        if (engine.Prewarm(&error)) {
            Log("model loaded");
        } else {
            // モデル未取得でも起動は継続する（変換要求時にエラーを返す）
            Log(("prewarm failed: " + error).c_str());
        }
    }

    const std::wstring pipeName = iroha::ipc::PipeName();
    Log("serving");

    bool running = true;
    while (running) {
        HANDLE pipe = CreateNamedPipeW(
            pipeName.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, iroha::ipc::kMaxMessageBytes,
            iroha::ipc::kMaxMessageBytes, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Log("CreateNamedPipe failed");
            return 1;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                                   ? TRUE
                                   : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            std::vector<char> request(iroha::ipc::kMaxMessageBytes);
            DWORD bytesRead = 0;
            if (ReadFile(pipe, request.data(), static_cast<DWORD>(request.size()),
                         &bytesRead, nullptr)) {
                request.resize(bytesRead);
                std::vector<char> response;
                running = HandleRequest(engine, request, &response);
                DWORD written = 0;
                WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()),
                          &written, nullptr);
                FlushFileBuffers(pipe);
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
    Log("shutdown");
    return 0;
}
