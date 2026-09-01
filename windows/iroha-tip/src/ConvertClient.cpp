#include "ConvertClient.h"

#include "Globals.h"
#include "ipc_protocol.h"
#include "iroha/unicode.h"

namespace ConvertClient {

namespace {

bool Ping(DWORD timeoutMs) {
    std::vector<char> response;
    return iroha::ipc::Call(iroha::ipc::BuildPingRequest(), &response, timeoutMs);
}

void SpawnServer() {
    // このDLLと同じディレクトリの iroha-server.exe を起動する
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(g_hInst, path, ARRAYSIZE(path));
    if (length == 0) return;
    std::wstring dllPath(path, length);
    const size_t slash = dllPath.find_last_of(L'\\');
    if (slash == std::wstring::npos) return;
    const std::wstring exe = dllPath.substr(0, slash + 1) + L"iroha-server.exe";

    std::wstring commandLine = L"\"" + exe + L"\"";
    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo = {};
    if (CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo,
                       &processInfo)) {
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        IrohaLog(L"spawned iroha-server");
    } else {
        IrohaLog(L"CreateProcess(iroha-server) failed: %u", GetLastError());
    }
}

} // namespace

void EnsureServer() {
    if (!Ping(500)) SpawnServer();
}

bool Convert(const std::u32string& reading, const std::u32string& context,
             int candidateCount, std::vector<std::u32string>* candidates) {
    candidates->clear();
    const std::vector<char> request = iroha::ipc::BuildConvertRequest(
        iroha::Utf32ToUtf8(reading), iroha::Utf32ToUtf8(context),
        static_cast<uint32_t>(candidateCount));

    std::vector<char> responseBytes;
    // 変換自体に時間がかかる（CPUで数百ms、コールドスタート時はモデルロード込み）
    if (!iroha::ipc::Call(request, &responseBytes, 15000)) {
        // サーバが落ちている可能性が高い。起動して少し待ってから1回だけ再試行
        SpawnServer();
        for (int i = 0; i < 20 && !Ping(500); ++i) Sleep(250);
        if (!iroha::ipc::Call(request, &responseBytes, 15000)) {
            IrohaLog(L"convert: server unreachable");
            return false;
        }
    }

    const iroha::ipc::ConvertResponse response =
        iroha::ipc::ParseConvertResponse(responseBytes);
    if (!response.ok) {
        IrohaLog(L"convert: server error");
        return false;
    }
    for (const auto& candidate : response.candidates) {
        candidates->push_back(iroha::Utf8ToUtf32(candidate));
    }
    return true;
}

} // namespace ConvertClient
