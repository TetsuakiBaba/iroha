#include "ConvertClient.h"

#include <vector>

#include "Globals.h"
#include "ipc_protocol.h"

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

bool ParseOutput(const std::vector<char>& bytes, bool* changed, iroha::SessionOutput* output) {
    iroha::ipc::SessionResponse response = iroha::ipc::ParseSessionResponse(bytes);
    if (!response.ok) {
        IrohaLog(L"session: server error");
        return false;
    }
    if (changed) *changed = response.changed;
    if (response.changed && output) *output = std::move(response.output);
    return true;
}

} // namespace

void EnsureServer() {
    if (!Ping(500)) SpawnServer();
}

bool SendKey(uint64_t sessionId, iroha::SessionKey key, char32_t character, bool shift,
             const std::u32string* leftContext, iroha::SessionOutput* output) {
    const std::vector<char> request =
        iroha::ipc::BuildSessionKeyRequest(sessionId, key, character, shift, leftContext);
    std::vector<char> responseBytes;
    // Space での変換・候補の取得はこの呼び出しの中で待つ（CPUで数百ms）
    if (!iroha::ipc::Call(request, &responseBytes, 15000)) {
        // サーバが落ちている可能性が高い。起動して少し待ってから1回だけ再試行
        SpawnServer();
        for (int i = 0; i < 20 && !Ping(500); ++i) Sleep(250);
        if (!iroha::ipc::Call(request, &responseBytes, 15000)) {
            IrohaLog(L"session: server unreachable");
            return false;
        }
    }
    bool changed = false;
    if (!ParseOutput(responseBytes, &changed, output)) return false;
    return changed; // キーへの返事には必ず表示が入っている
}

void ResetSession(uint64_t sessionId) {
    std::vector<char> response;
    iroha::ipc::Call(iroha::ipc::BuildSessionKeyRequest(sessionId, iroha::SessionKey::Reset, 0,
                                                        false, nullptr),
                     &response, 500, /*attempts=*/1);
}

bool Poll(uint64_t sessionId, uint64_t revision, bool* changed, iroha::SessionOutput* output) {
    *changed = false;
    std::vector<char> responseBytes;
    if (!iroha::ipc::Call(iroha::ipc::BuildSessionPollRequest(sessionId, revision),
                          &responseBytes, 200, /*attempts=*/1)) {
        return false;
    }
    return ParseOutput(responseBytes, changed, output);
}

void EndSession(uint64_t sessionId) {
    std::vector<char> response;
    iroha::ipc::Call(iroha::ipc::BuildSessionEndRequest(sessionId), &response, 500,
                     /*attempts=*/1);
}

} // namespace ConvertClient
