// iroha変換サーバ。
// 名前付きパイプでTIP（各アプリのプロセス内）からの変換要求を受け、
// ZenzEngine（llama.cpp + GGUFモデル）で変換して返す。
//
// - 単一インスタンス（セッション別の名前付きミューテックスでガード）
// - 単一スレッドで1接続ずつ処理する（エンジンの直列化を兼ねる）
// - モデルは起動時にプリロードする（初回変換のもたつき防止）

#include "ipc_protocol.h"

#include <sddl.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "iroha/conversion_engine.h"
#include "iroha/learning_engine.h"
#include "iroha/learning_store.h"
#include "iroha/reading_aligner.h"
#include "iroha/unicode.h"
#include "iroha/user_dictionary_engine.h"
#include "iroha/user_dictionary_store.h"
#include "iroha/zenz_engine.h"

namespace {

void Log(const char* message) {
    std::printf("%s\n", message);
    OutputDebugStringA((std::string("[iroha-server] ") + message + "\n").c_str());
}

std::wstring DataDir() {
    const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
    return (localAppData ? std::wstring(localAppData) : L".") + L"\\iroha";
}

std::string DefaultModelPath() {
    if (const wchar_t* env = _wgetenv(L"IROHA_MODEL")) {
        return iroha::Utf32ToUtf8(iroha::Utf16ToUtf32(env));
    }
    return iroha::Utf32ToUtf8(
        iroha::Utf16ToUtf32(DataDir() + L"\\models\\zenz-v3.1-small-Q5_K_M.gguf"));
}

std::filesystem::path StorePath(const wchar_t* envName, const wchar_t* fileName) {
    if (const wchar_t* env = _wgetenv(envName)) return std::filesystem::path(env);
    return std::filesystem::path(DataDir() + L"\\" + fileName);
}

// ストアアプリ（AppContainer・低整合性）内のTIPからも接続できるパイプの
// セキュリティ記述子を作る:
//   - 実行ユーザにフルアクセス
//   - ALL APPLICATION PACKAGES / ALL RESTRICTED APPLICATION PACKAGES に読み書き
//   - 低整合性レベルからのアクセスを許可（UWPはLow ILで動く）
PSECURITY_DESCRIPTOR CreatePipeSecurityDescriptor() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;
    BYTE buffer[256] = {};
    DWORD length = 0;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &length)) {
        LPWSTR userSid = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid,
                                   &userSid)) {
            wchar_t sddl[512];
            _snwprintf_s(sddl, _TRUNCATE,
                         L"D:(A;;GA;;;%s)"
                         L"(A;;GRGW;;;S-1-15-2-1)"
                         L"(A;;GRGW;;;S-1-15-2-2)"
                         L"S:(ML;;NW;;;LW)",
                         userSid);
            LocalFree(userSid);
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
                descriptor = nullptr;
            }
        }
    }
    CloseHandle(token);
    return descriptor;
}

// IME本体と同じ構成のエンジン一式: 学習 → ユーザ辞書 → zenz
struct ServerState {
    iroha::ConversionEngine* engine = nullptr;
    iroha::LearningStore* learning = nullptr;
};

// 1リクエストを処理してレスポンスのバイト列を返す。shutdownならfalseを返す
bool HandleRequest(ServerState& state, const std::vector<char>& request,
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
            if (!state.engine->Convert(iroha::Utf8ToUtf32(readingUtf8),
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
        case iroha::ipc::MessageType::Record: {
            std::string readingUtf8;
            std::string committedUtf8;
            std::string baselineUtf8;
            if (!reader.String(&readingUtf8) || !reader.String(&committedUtf8) ||
                !reader.String(&baselineUtf8)) {
                *response = iroha::ipc::BuildErrorResponse("プロトコルが不正です");
                return true;
            }
            // エンジンの出力をそのまま確定した場合は何も覚えない（macOS版と同じ）
            if (committedUtf8 != baselineUtf8 && !committedUtf8.empty()) {
                const std::u32string reading = iroha::Utf8ToUtf32(readingUtf8);
                const std::u32string committed = iroha::Utf8ToUtf32(committedUtf8);
                // 文節UIがまだないため、文節の内訳はReadingAlignerの推定で近似する
                const auto segments =
                    iroha::ReadingAligner::SegmentReading(reading, committed);
                std::vector<iroha::LearningStore::SegmentPair> pairs;
                for (const auto& segment : segments) {
                    pairs.push_back({segment.reading, segment.conversion});
                }
                state.learning->Record(reading, committed, pairs);
                Log("learned");
            }
            *response = iroha::ipc::BuildOkResponse({});
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

    // IME本体と同じ構成でエンジンを組み立てる: 学習 → ユーザ辞書 → zenz
    // （macOS版 iroha-cli の makeEngine と同じ。環境変数で差し替え可能）
    iroha::ZenzEngine zenz(DefaultModelPath());
    iroha::UserDictionaryStore userDictionary(
        StorePath(L"IROHA_USER_DICT", L"user-dictionary.json"));
    iroha::LearningStore learning(StorePath(L"IROHA_LEARNING", L"learning.json"));
    iroha::UserDictionaryEngine dictionaryEngine(
        &zenz, [&userDictionary] { return userDictionary.Current(); });
    iroha::LearningEngine engine(&dictionaryEngine,
                                 [&learning] { return learning.Current(); });
    ServerState state{&engine, &learning};
    {
        std::string error;
        if (zenz.Prewarm(&error)) {
            Log("model loaded");
        } else {
            // モデル未取得でも起動は継続する（変換要求時にエラーを返す）
            Log(("prewarm failed: " + error).c_str());
        }
    }

    const std::wstring pipeName = iroha::ipc::PipeName();
    PSECURITY_DESCRIPTOR pipeSecurity = CreatePipeSecurityDescriptor();
    if (!pipeSecurity) Log("pipe SD unavailable (falling back to default ACL)");
    SECURITY_ATTRIBUTES securityAttributes = {};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.lpSecurityDescriptor = pipeSecurity;
    Log("serving");

    bool running = true;
    while (running) {
        HANDLE pipe = CreateNamedPipeW(
            pipeName.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, iroha::ipc::kMaxMessageBytes,
            iroha::ipc::kMaxMessageBytes, 0,
            pipeSecurity ? &securityAttributes : nullptr);
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
                running = HandleRequest(state, request, &response);
                DWORD written = 0;
                WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()),
                          &written, nullptr);
                FlushFileBuffers(pipe);
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
    if (pipeSecurity) LocalFree(pipeSecurity);
    Log("shutdown");
    return 0;
}
