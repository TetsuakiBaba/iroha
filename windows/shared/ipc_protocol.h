#pragma once
// TIP ⇔ 変換サーバ間の名前付きパイプIPCプロトコル（ヘッダオンリー）。
//
// - パイプ名はセッション別（マルチユーザ環境での衝突を避ける）
// - メッセージモードのパイプで1リクエスト=1接続（CallNamedPipe）。
//   サーバは単一スレッドで順に処理するため、エンジンの直列化を兼ねる
// - ワイヤ形式はリトルエンディアンのu32と長さ付きUTF-8文字列のみ
//   （JSONパーサ等の依存を持ち込まない）
//
//   リクエスト:  u32 version, u32 type,
//                type==Convert のとき u32 n, string reading, string context
//   レスポンス:  u32 version, u32 status(0=ok),
//                ok: u32 count, string×count / エラー: string message

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace iroha::ipc {

constexpr uint32_t kProtocolVersion = 1;
constexpr DWORD kMaxMessageBytes = 64 * 1024;

enum class MessageType : uint32_t {
    Ping = 1,
    Convert = 2,
    Shutdown = 3,
};

inline std::wstring PipeName() {
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    return L"\\\\.\\pipe\\iroha-server-" + std::to_wstring(sessionId);
}

// ---- シリアライズ ----

inline void PutU32(std::vector<char>* buf, uint32_t value) {
    buf->push_back(static_cast<char>(value & 0xFF));
    buf->push_back(static_cast<char>((value >> 8) & 0xFF));
    buf->push_back(static_cast<char>((value >> 16) & 0xFF));
    buf->push_back(static_cast<char>((value >> 24) & 0xFF));
}

inline void PutString(std::vector<char>* buf, const std::string& s) {
    PutU32(buf, static_cast<uint32_t>(s.size()));
    buf->insert(buf->end(), s.begin(), s.end());
}

struct Reader {
    const char* data;
    size_t size;
    size_t pos = 0;

    bool U32(uint32_t* value) {
        if (pos + 4 > size) return false;
        *value = static_cast<uint32_t>(static_cast<unsigned char>(data[pos])) |
                 (static_cast<uint32_t>(static_cast<unsigned char>(data[pos + 1])) << 8) |
                 (static_cast<uint32_t>(static_cast<unsigned char>(data[pos + 2])) << 16) |
                 (static_cast<uint32_t>(static_cast<unsigned char>(data[pos + 3])) << 24);
        pos += 4;
        return true;
    }

    bool String(std::string* s) {
        uint32_t length = 0;
        if (!U32(&length)) return false;
        if (pos + length > size) return false;
        s->assign(data + pos, length);
        pos += length;
        return true;
    }
};

inline std::vector<char> BuildPingRequest() {
    std::vector<char> buf;
    PutU32(&buf, kProtocolVersion);
    PutU32(&buf, static_cast<uint32_t>(MessageType::Ping));
    return buf;
}

inline std::vector<char> BuildShutdownRequest() {
    std::vector<char> buf;
    PutU32(&buf, kProtocolVersion);
    PutU32(&buf, static_cast<uint32_t>(MessageType::Shutdown));
    return buf;
}

inline std::vector<char> BuildConvertRequest(const std::string& readingUtf8,
                                             const std::string& contextUtf8,
                                             uint32_t candidateCount) {
    std::vector<char> buf;
    PutU32(&buf, kProtocolVersion);
    PutU32(&buf, static_cast<uint32_t>(MessageType::Convert));
    PutU32(&buf, candidateCount);
    PutString(&buf, readingUtf8);
    PutString(&buf, contextUtf8);
    return buf;
}

inline std::vector<char> BuildOkResponse(const std::vector<std::string>& candidates) {
    std::vector<char> buf;
    PutU32(&buf, kProtocolVersion);
    PutU32(&buf, 0);
    PutU32(&buf, static_cast<uint32_t>(candidates.size()));
    for (const auto& candidate : candidates) PutString(&buf, candidate);
    return buf;
}

inline std::vector<char> BuildErrorResponse(const std::string& message) {
    std::vector<char> buf;
    PutU32(&buf, kProtocolVersion);
    PutU32(&buf, 1);
    PutString(&buf, message);
    return buf;
}

// ---- クライアント ----

// 1リクエストを送って応答を受け取る。サーバが別クライアントを処理中の場合は
// CallNamedPipeが内部で待つ。パイプ再作成の隙間はリトライで吸収する。
inline bool Call(const std::vector<char>& request, std::vector<char>* response,
                 DWORD timeoutMs) {
    const std::wstring name = PipeName();
    response->assign(kMaxMessageBytes, 0);
    for (int attempt = 0; attempt < 3; ++attempt) {
        DWORD bytesRead = 0;
        if (CallNamedPipeW(name.c_str(), const_cast<char*>(request.data()),
                           static_cast<DWORD>(request.size()), response->data(),
                           static_cast<DWORD>(response->size()), &bytesRead,
                           timeoutMs)) {
            response->resize(bytesRead);
            return true;
        }
        if (GetLastError() == ERROR_FILE_NOT_FOUND && attempt + 1 < 3) {
            Sleep(200); // サーバのパイプ再作成待ち
            continue;
        }
        break;
    }
    response->clear();
    return false;
}

struct ConvertResponse {
    bool ok = false;
    std::vector<std::string> candidates; // UTF-8
    std::string error;
};

inline ConvertResponse ParseConvertResponse(const std::vector<char>& bytes) {
    ConvertResponse result;
    Reader reader{bytes.data(), bytes.size()};
    uint32_t version = 0;
    uint32_t status = 0;
    if (!reader.U32(&version) || version != kProtocolVersion ||
        !reader.U32(&status)) {
        result.error = "protocol error";
        return result;
    }
    if (status != 0) {
        reader.String(&result.error);
        return result;
    }
    uint32_t count = 0;
    if (!reader.U32(&count)) {
        result.error = "protocol error";
        return result;
    }
    for (uint32_t i = 0; i < count; ++i) {
        std::string candidate;
        if (!reader.String(&candidate)) {
            result.error = "protocol error";
            return result;
        }
        result.candidates.push_back(std::move(candidate));
    }
    result.ok = true;
    return result;
}

} // namespace iroha::ipc
